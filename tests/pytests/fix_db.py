import time
import os
import logging
from datetime import datetime, timedelta
from threading import Thread

import pytest
import psycopg2

from replisome.errors import ReplisomeError

logger = logging.getLogger(__name__)


@pytest.fixture
def src_db():
    "Return the source database to connect for testing"
    dsn = os.environ.get(
        "RS_TEST_SRC_DSN",
        'host=localhost dbname=rs_src_test')
    db = TestDatabase(dsn, slot='rs_test_slot')

    # Create the extension to make the version number available
    with db.make_conn() as cnn:
        cur = cnn.cursor()
        cur.execute('drop extension if exists replisome')
        cur.execute('create extension replisome')
    cnn.close()

    yield db
    db.teardown()


@pytest.fixture
def tgt_db():
    "Return the target database to connect for testing"
    dsn = os.environ.get(
        "RS_TEST_TGT_DSN",
        'host=localhost dbname=rs_tgt_test')
    db = TestDatabase(dsn)
    yield db
    db.teardown()


class TestDatabase(object):
    """
    The representation of a database used for testing.

    The database can be the sender of the receiver: a few methods may make
    sense only in one case.

    The object optionally manages teardown for one replication slot.
    """
    def __init__(self, dsn, slot=None):
        self.dsn = dsn
        self.slot = slot
        self.plugin = 'replisome'

        self._conn = None
        self._conns = []
        self._threads = []

    @property
    def conn(self):
        """
        A connection to the database.

        The object is always the same for the object lifetime.
        """
        if not self._conn:
            self._conn = self.make_conn()
        return self._conn

    def teardown(self):
        """
        Close the database connections and stop any receiving thread.

        Invoked at the end of the tests.
        """
        for thread in self._threads:
            if thread is not None:
                thread.stop()
                thread.join()

        for cnn in self._conns:
            cnn.close()

        if self.slot:
            logger.debug('Dropping replication slot')
            self.drop_slot()

    def make_conn(self, autocommit=True, **kwargs):
        """Create a new connection to the test database.

        The connection is autocommit, and will be closed on teardown().
        """
        cnn = psycopg2.connect(self.dsn, **kwargs)
        cnn.autocommit = autocommit
        self._conns.append(cnn)
        return cnn

    def drop_slot(self):
        """Delete the replication slot with the current name if exists."""
        with self.make_conn() as cnn:
            cur = cnn.cursor()
            cur.execute("""
                select pg_drop_replication_slot(slot_name)
                from pg_replication_slots
                where slot_name = %s
                """, (self.slot,))

        # Closing explicitly as the function can be called in teardown, after
        # other connections (maybe using the slot) have been closed.
        cnn.close()

    def run_receiver(self, receiver, dsn):
        """
        Run a receiver loop in a thread.

        Stop the receiver at the end of the test.
        """
        receiver.dsn = dsn
        return self.add_thread(receiver)

    def run_pipeline(self, pipeline):
        """
        Runs the given pipeline in a thread. Stops pipeline at end of the test.

        :param: pipeline
        """
        return self.add_thread(pipeline)

    def add_thread(self, obj, timeout=timedelta(seconds=1)):
        thread = Thread(target=obj.start)
        thread.stop = obj.stop
        thread.start()
        start_time = datetime.utcnow()
        while not obj.is_running:
            if (start_time + timeout) < datetime.utcnow():
                raise TimeoutError()
            time.sleep(0.01)
        self._threads.append(thread)
        return len(self._threads) - 1

    def remove_thread(self, thread_index):
        thread = self._threads[thread_index]
        thread.stop()
        thread.join()
        self._threads[thread_index] = None

