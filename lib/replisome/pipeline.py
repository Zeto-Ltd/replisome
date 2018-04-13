
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
        self.blocking = False

    def __del__(self):
        if self.state == self.RUNNING:
            self.stop()

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

        self.state = self.RUNNING
        self.blocking = kwargs.get('block', True)
        self.receiver.start(**kwargs)

    def on_loop(self, *args, **kwargs):
        self.receiver.on_loop(*args, **kwargs)

    def stop(self):
        if self.state != self.RUNNING:
            raise ValueError("can't stop pipeline in state %s" % self.state)

        if self.blocking:
            self.receiver.stop_blocking()
        else:
            self.receiver.close()
        self.state = self.STOPPED

    def process_message(self, msg):
        for f in self.filters:
            msg = f(msg)
            if msg is None:
                return

        self.consumer(msg)
