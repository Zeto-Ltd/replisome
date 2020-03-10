import json

from .base import BaseReceiver

__all__ = ['Wal2JsonReceiver']


class Wal2JsonReceiver(BaseReceiver):

    @classmethod
    def from_config(cls, config):
        opts = []
        for k in ('include-xids', 'include-timestamp', 'include-schemas',
                  'include-types', 'include-typmod', 'include-type-oids',
                  'include-not-null', 'pretty-print', 'include-lsn',
                  'filter-tables', 'add-tables'):
            if k in config:
                v = config.pop(k)
                opts.append((k, (v and 't' or 'f')))

        return cls(options=opts)

    def process_payload(self, raw_payload):
        payload = raw_payload.decode('utf-8')
        self.logger.debug(
            'next chunk payload:\n\t%s%s',
            payload[:300], len(payload) > 300 and '...' or '')
        parsed_payload = json.loads(payload)
        for change in parsed_payload['change']:
            self.message_cb(change)
