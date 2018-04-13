from __future__ import absolute_import

import json

import psycopg2

from replisome.errors import ConfigError, ReplisomeError
from .base import BaseReceiver
from replisome.version import check_version

__all__ = ['JsonReceiver']


class JsonReceiver(BaseReceiver):

    def __init__(self, *args, **kwargs):
        super(JsonReceiver, self).__init__(*args, **kwargs)
        self._chunks = []

    def verify(self):
        cnn = psycopg2.connect(self.dsn)
        cur = cnn.cursor()
        try:
            cur.execute('select replisome_version()')
        except psycopg2.ProgrammingError as e:
            if e.pgcode == '42883':     # function not found
                raise ReplisomeError("function replisome_version() not found")
            else:
                raise
        else:
            ver = cur.fetchone()[0]
        finally:
            cnn.rollback()
            cnn.close()

        check_version(ver)

    @classmethod
    def from_config(cls, config):
        opts = []
        # TODO: make them the same (parse the underscore version in the plugin)
        for k in ('pretty_print', 'include_xids', 'include_lsn',
                  'include_timestamp', 'include_schemas', 'include_types',
                  'include_empty_xacts'):
            if k in config:
                v = config.pop(k)
                opts.append((k.replace('_', '-'), (v and 't' or 'f')))

        incs = config.pop('includes', [])
        if not isinstance(incs, list):
            raise ConfigError('includes should be a list, got %s' % (incs,))

        for inc in incs:
            # TODO: this might be parsed by the plugin
            k = inc.pop('exclude', False) and 'exclude' or 'include'
            opts.append((k, json.dumps(inc)))

        if config:
            raise ConfigError(
                "unknown %s option entries: %s" %
                (cls.__name__, ', '.join(sorted(config))))

        # NOTE: it is currently necessary to write in chunk even if this
        # results in messages containing invalid JSON (which are assembled
        # by consume()). If we don't do so, it seems postgres fails to flush
        # the lsn correctly. I suspect the problem is that we reset the lsn
        # at the start of the message: if the output is chunked we receive
        # at least a couple of messages within the same transaction so we
        # end up being correct. If the message encompasses the entire
        # transaction, the lsn is reset at the beginning of the transaction
        # already seen, so some records are sent repeatedly.
        opts = [('write-in-chunks', '1')] + opts
        return cls(options=opts)

    def process_payload(self, raw_payload):
        # FIXME: what happens here if non-ASCII chars are in the JSON payload?
        chunk = raw_payload.decode('ascii')
        self.logger.debug(
            "message received:\n\t%s%s",
            chunk[:70], len(chunk) > 70 and '...' or '')
        self._chunks.append(chunk)

        # attempt parsing each chunk as part of the whole
        try:
            obj = json.loads(''.join(self._chunks))
            del self._chunks[:]
            self.message_cb(obj)
        except ValueError:
            pass
