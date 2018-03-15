from .base import BaseReceiver

__all__ = ['TestDecodingReceiver']


class TestDecodingReceiver(BaseReceiver):

    def process_payload(self, raw_payload):
        payload = raw_payload.decode('utf-8')
        self.logger.debug(
            'next chunk payload:\n\t%s%s',
            payload[:300], len(payload) > 300 and '...' or '')
        self.message_cb(payload)
