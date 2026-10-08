#!/usr/bin/env python3
"""Fetch exact public API headers and opaque vendor binaries by Git blob ID.

No driver or codec implementation source is downloaded.
"""
import base64
import hashlib
import json
from pathlib import Path
import urllib.request

B=Path(__file__).resolve().parent
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
for entry in json.loads((B/'sdk-provenance.json').read_text()):
    with opener.open(entry['url'],timeout=40) as response:
        obj=json.load(response)
    data=base64.b64decode(obj['content'])
    sha=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
    if sha!=entry['git_blob_sha1'] or hashlib.sha256(data).hexdigest()!=entry['sha256']:
        raise ValueError('SDK object mismatch: '+entry['path'])
    path=B/entry['local'];path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
    print(entry['path'],len(data),flush=True)
