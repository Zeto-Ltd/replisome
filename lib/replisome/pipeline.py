import logging

logger = logging.getLogger(__name__)


class Pipeline(object):
    """A chain of operations on a stream of changes"""
    NOT_STARTED = 'NOT_STARTED'
    RUNNING = 'RUNNING'
    STOPPED = 'STOPPED'

    def __init__(self):
        self._receiver = None
        self.filters = []
        self.consumer = None
        self.state = self.NOT_STARTED

    def __del__(self):
        if self.state == self.RUNNING:
            self.stop()

    @property
    def is_running(self):
        return self.state == self.RUNNING and \
               ((self.receiver.is_blocking and self.receiver.is_running) or
                not self.receiver.is_blocking)

    @property
    def receiver(self):
        return self._receiver

    @receiver.setter
    def receiver(self, new_receiver):
        new_receiver.verify()
        new_receiver.message_cb = self.process_message
        self._receiver = new_receiver

    def start(self, **kwargs):
        if self.state != self.NOT_STARTED:
            raise ValueError("can't start pipeline in state %s" % self.state)

        if not self.receiver:
            raise ValueError("can't start: no receiver")

        if not self.consumer:
            raise ValueError("can't start: no consumer")

        logger.debug('Starting receiver with args %s', kwargs)
        self.receiver.is_blocking = kwargs.pop('block', True)
        if self.receiver.is_blocking:
            self.state = self.RUNNING
        self.receiver.start(**kwargs)
        if not self.receiver.is_blocking:
            self.state = self.RUNNING

    def on_loop(self, *args, **kwargs):
        self.receiver.on_loop(*args, **kwargs)

    def stop(self):
        if self.state != self.RUNNING:
            raise ValueError("can't stop pipeline in state %s" % self.state)

        self.receiver.stop()
        self.state = self.STOPPED

    def process_message(self, msg):
        for f in self.filters:
            msg = f(msg)
            if msg is None:
                return

        self.consumer(msg)
