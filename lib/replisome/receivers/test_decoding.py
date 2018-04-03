from .base import BaseReceiver

__all__ = ['TestDecodingReceiver']


class TestDecodingReceiver(BaseReceiver):

    @classmethod
    def from_config(cls, config):
        opts = []
        for k in ('include-xids', 'include-timestamp', 'skip-empty-xacts',
                  'only-local', 'include-rewrites', 'force-binary'):
            if k in config:
                v = config.pop(k)
                opts.append((k, (v and 't' or 'f')))

        return cls(options=opts)

    def process_payload(self, raw_payload):
        payload = raw_payload.decode('utf-8')
        self.logger.debug(
            'next chunk payload:\n\t%s%s',
            payload[:300], len(payload) > 300 and '...' or '')
        self.message_cb(payload)
