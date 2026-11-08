#!/usr/bin/env python3
# SPDX-License-Identifier: CDDL-1.0
"""
Convert NIST CAVP GCM response files (gcmEncryptExtIV*.rsp,
gcmDecrypt*.rsp from gcmtestvectors.zip) into the test file format of
tests/zfs-tests/cmd/crypto_test.  Bench/test only, not for upstream.

A decrypt vector marked FAIL carries no plaintext.  It becomes an
"invalid" test whose message is zeros of the ciphertext's length:
crypto_test then requires decryption to be refused, and the tag that
encryption of those zeros produces to differ from the given one.
"""
import sys


def convert(paths, out):
	tests = []
	for path in paths:
		name = path.rsplit('/', 1)[-1]
		params = {}
		cur = None
		with open(path) as fh:
			lines = [ln.strip() for ln in fh] + ['']
		for ln in lines:
			if ln.startswith('#'):
				continue
			if ln.startswith('[') and ln.endswith(']'):
				k, _, v = ln[1:-1].partition('=')
				params[k.strip()] = v.strip()
				continue
			if ln == '':
				if cur is not None and 'Count' in cur:
					tests.append((name, dict(params), cur))
				cur = None
				continue
			if ln == 'FAIL':
				cur['FAIL'] = True
				continue
			k, _, v = ln.partition('=')
			if cur is None:
				cur = {}
			cur[k.strip()] = v.strip()
	out.write('algorithm: AES-GCM\ntests: %d\n' % len(tests))
	for i, (name, p, t) in enumerate(tests, 1):
		ct = t.get('CT', '')
		fail = t.get('FAIL', False)
		msg = t.get('PT', '0' * len(ct) if fail else '')
		comment = '%s Keylen=%s IVlen=%s PTlen=%s AADlen=%s ' \
		    'Taglen=%s Count=%s' % (name, p.get('Keylen'),
		    p.get('IVlen'), p.get('PTlen'), p.get('AADlen'),
		    p.get('Taglen'), t['Count'])
		out.write('\nid: %d\ncomment: %s\nflags: %s\n' %
		    (i, comment, 'NIST-FAIL' if fail else 'NIST'))
		out.write('iv: %s\nkey: %s\nmsg: %s\nct: %s\naad: %s\n'
		    'tag: %s\nresult: %s\n' % (t['IV'], t['Key'], msg, ct,
		    t.get('AAD', ''), t['Tag'],
		    'invalid' if fail else 'valid'))
	return len(tests)


if __name__ == '__main__':
	n = convert(sys.argv[1:], sys.stdout)
	print('%d vectors' % n, file=sys.stderr)
