from datetime import datetime, timedelta
from select import select
import logging
import os

import psycopg2
from psycopg2.extras import LogicalReplicationConnection, wait_select
from psycopg2 import sql


class BaseReceiver(object):

    def __init__(self, slot=None, dsn=None, message_cb=None, block=True,
                 plugin='replisome', options=None, flush_interval=10,
                 block_wait=0.1):
        self.slot = slot
        self.dsn = dsn
        self.plugin = plugin
        self.options = options or []
        if message_cb:
            self.message_cb = message_cb
        self.logger = logging.getLogger(
            'replisome.{}'.format(self.__class__.__name__))
        self.connection = None
        self.cursor = None
        self.is_running = False
        self.is_blocking = block
        self._shutdown_pipe = os.pipe()
        self.next_wait_time = None
        self.blocking_wait = block_wait
        self.flush_delta = None
        if flush_interval > 0:
            self.flush_delta = timedelta(seconds=flush_interval)

    def verify(self):
        """
        Verifies that the receiver is correctly configured and raises error if
        any issues found. May check server for installed plugins, etc.

        :raises ReplisomeError: if verification fails
        """
        pass

    @classmethod
    def from_config(cls, config):
        return cls(options={})

    def __del__(self):
        self.stop()

    def stop(self):
        if self.is_blocking:
            self.is_running = False
        os.write(self._shutdown_pipe[1], b'stop')
        if not self.is_blocking:
            self.close(flush=True)

    def close(self, flush=False):
        self.logger.info('Closing DB connection for %s',
                         self.__class__.__name__)
        if self.cursor:
            try:
                if flush:
                    # support final flush on clean shutdown
                    self.cursor.send_feedback()
                self.cursor.close()
            except Exception:
                self.logger.exception('Failed to close connection cursor')
            self.cursor = None
        if self.connection:
            try:
                self.connection.close()
            except Exception:
                self.logger.exception('Failed to close connection')
            self.connection = None

    def update_status_time(self):
        if self.flush_delta is not None:
            self.next_wait_time = datetime.utcnow() + self.flush_delta

    def start(self, lsn=None):
        if not self.slot:
            raise AttributeError('no slot specified')

        self.create_slot()
        if lsn is None:
            lsn = self.get_restart_lsn()
        self.create_connection()

        self.logger.info('starting streaming from slot "%s" at LSN %s',
                         self.slot, lsn)
        stmt = self._get_replication_statement(self.connection, lsn)
        self.cursor.start_replication_expert(stmt, decode=False)
        wait_select(self.connection)

        self.update_status_time()
        if self.is_blocking:
            self.logger.debug('Listening to replication slot %s', self.slot)
            self.is_running = True
            try:
                while self.is_running:
                    self.on_loop()
                    self.wait_for_data(timeout=self.blocking_wait)
            finally:
                self.close(flush=not self.is_running)

    def wait_for_data(self, timeout=0.1):
        select([self._shutdown_pipe[0], self.connection], [], [], timeout)

    def on_loop(self):
        msg = self.cursor.read_message()
        if msg:
            self.consume(msg)
        if self.flush_delta is None or self.next_wait_time < datetime.utcnow():
            flush_lsn = msg.wal_end if msg else 0
            self.cursor.send_feedback(flush_lsn=flush_lsn, reply=True)
            wait_select(self.connection)
            self.update_status_time()

    def _get_replication_statement(self, cnn, lsn):
        bits = [
            sql.SQL('START_REPLICATION SLOT '),
            sql.Identifier(self.slot),
            sql.SQL(' LOGICAL '),
            sql.SQL(lsn)]

        if self.options:
            bits.append(sql.SQL(' ('))
            for k, v in self.options:
                bits.append(sql.Identifier(k))
                if v is not None:
                    bits.append(sql.SQL(' '))
                    bits.append(sql.Literal(v))
                bits.append(sql.SQL(', '))
            bits[-1] = sql.SQL(')')

        rv = sql.Composed(bits).as_string(cnn)
        self.logger.debug('replication statement: %s', rv)
        return rv

    def process_payload(self, raw_payload):
        """
        Converts the raw payload bytes from the most recent replication chunk
        and invokes the message callback if appropriate

        :param raw_payload: bytes object containing most recent chunk payload
        """
        raise NotImplementedError(
            'Missing `process_payload` definition for receiver {}'.
            format(self.__class__.__name__))

    def consume(self, raw_chunk):
        if self.connection.notices:
            for n in self.connection.notices:
                self.logger.debug('server: %s', n.rstrip())
            del self.connection.notices[:]

        self.process_payload(raw_chunk.payload)

    def message_cb(self, obj):
        self.logger.info('message received: %s', obj)

    def create_connection(self):
        self.logger.info('connecting to source database at "%s"', self.dsn)
        cnn = psycopg2.connect(
            self.dsn, async_=True,
            connection_factory=LogicalReplicationConnection)
        wait_select(cnn)
        self.connection = cnn
        self.cursor = cnn.cursor()

    def get_restart_lsn(self):
        """
        Returns the restart LSN for the replication slot as stored in the
        pg_replication_slots DB table. Returns default start position if
        slot doesn't exist.

        :return: string containing restart LSN for current slot
        """
        command = '''
        SELECT restart_lsn FROM pg_replication_slots WHERE slot_name = %s
        '''
        lsn = '0/0'
        try:
            with psycopg2.connect(self.dsn) as conn, conn.cursor() as cursor:
                cursor.execute(command, [self.slot])
                result = cursor.fetchone()
                if result:
                    lsn = result[0]
        except psycopg2.Error as e:
            self.logger.error('error retrieving LSN: %s', e)
        return lsn

    def create_slot(self):
        """
        Creates the replication slot, if it hasn't been created already.
        """
        self.logger.info('creating replication slot "%s" with plugin %s',
                         self.slot, self.plugin)
        command = '''
WITH new_slots(slot_name) AS (
    VALUES(%s)
)
SELECT CASE WHEN slots.slot_name IS NULL THEN
       pg_create_logical_replication_slot(new_slots.slot_name, %s)
       ELSE NULL
       END
FROM new_slots
  LEFT JOIN (SELECT slot_name
             FROM pg_replication_slots
             WHERE slot_name = %s) slots
  ON slots.slot_name = new_slots.slot_name
'''
        try:
            # must use separate connection as main replication connection
            # doesn't support the custom query
            with psycopg2.connect(self.dsn) as conn, conn.cursor() as cursor:
                cursor.execute(command, (self.slot, self.plugin, self.slot))
        except psycopg2.Error as e:
            self.logger.error('error creating replication slot: %s', e)

    def drop_slot(self):
        self.logger.info('dropping replication slot "%s"', self.slot)
        command = '''
SELECT pg_drop_replication_slot(slot_name)
FROM pg_replication_slots
WHERE slot_name = %s
'''
        try:
            with psycopg2.connect(self.dsn) as conn, conn.cursor() as cursor:
                cursor.execute(command, [self.slot])
        except Exception as e:
            self.logger.error('error dropping replication slot: %s', e)
