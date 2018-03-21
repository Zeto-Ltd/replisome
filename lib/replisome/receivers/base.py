import os
from select import select
import logging

import psycopg2
from psycopg2.extras import LogicalReplicationConnection, wait_select
from psycopg2 import sql


class BaseReceiver(object):

    def __init__(self, slot=None, dsn=None, message_cb=None,
                 plugin='replisome', options=None):
        self.slot = slot
        self.dsn = dsn
        self.plugin = plugin
        self.options = options or []
        if message_cb:
            self.message_cb = message_cb
        self.logger = logging.getLogger(
            'replisome.{}'.format(self.__class__.__name__))

        self._shutdown_pipe = os.pipe()

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

    def start(self, slot_create=False, lsn=None, block=True):
        if not self.slot:
            raise ValueError('no slot specified')

        if slot_create:
            self.create_slot()

        connection = self.create_connection()
        if not connection.async_:
            raise ValueError('the connection should be asynchronous')

        cur = connection.cursor()
        stmt = self._get_replication_statement(connection, lsn)

        self.logger.info(
            'starting streaming from slot "%s"', self.slot)
        cur.start_replication_expert(stmt, decode=False)
        wait_select(connection)

        if block:
            while self.on_loop(connection, cur):
                pass

        cur.close()
        return connection

    def on_loop(self, connection, cursor):
        is_running = True
        msg = cursor.read_message()
        if msg:
            self.consume(msg)
        else:
            # TODO: handle InterruptedError
            sel = select(
                [self._shutdown_pipe[0], connection], [], [], 10)
            if not any(sel):
                cursor.send_feedback()
            elif self._shutdown_pipe[0] in sel[0]:
                is_running = False
        return is_running

    def stop(self):
        os.write(self._shutdown_pipe[1], b'stop')

    def _get_replication_statement(self, cnn, lsn):
        bits = [
            sql.SQL('START_REPLICATION SLOT '),
            sql.Identifier(self.slot),
            sql.SQL(' LOGICAL '),
            sql.SQL(lsn or '0/0')]

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
        cnn = raw_chunk.cursor.connection
        if cnn.notices:
            for n in cnn.notices:
                self.logger.debug('server: %s', n.rstrip())
            del cnn.notices[:]

        self.process_payload(raw_chunk.payload)
        raw_chunk.cursor.send_feedback(flush_lsn=raw_chunk.data_start)

    def message_cb(self, obj):
        self.logger.info('message received: %s', obj)

    def create_connection(self, async_=True):
        self.logger.info('connecting to source database at "%s"', self.dsn)
        cnn = psycopg2.connect(
            self.dsn, async_=async_,
            connection_factory=LogicalReplicationConnection)
        wait_select(cnn)
        return cnn

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
            self.logger.error(
                'error dropping replication slot: %s', e)

    def drop_slot(self):
        try:
            with self.create_connection(async_=False) as cnn:
                cur = cnn.cursor()
                self.logger.info('dropping replication slot "%s"', self.slot)
                cur.drop_replication_slot(self.slot)
        except Exception as e:
            self.logger.error(
                'error dropping replication slot: %s', e)

    def close(self, connection):
        try:
            connection.close()
        except Exception as e:
            self.logger.error(
                'error closing receiving connection - ignoring: %s', e)
