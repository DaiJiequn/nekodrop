# Applies nekodrop's changes to the staged copy of winawdl's AirDrop engine.
# Usage: python patch-engine.py <staged airdrop folder>
#
# - A stable mDNS instance name (NEKODROP_SERVICE_ID, kept by the client), so
#   an engine restart (a new awdl0 after the radio restarts) shows up on the
#   iPhone as the same device instead of a second one.
import pathlib
import sys

engine = pathlib.Path(sys.argv[1]) / 'engine.py'
text = engine.read_text(encoding='utf-8')
old = 'service_id = secrets.token_hex(6)'
new = "service_id = os.environ.get('NEKODROP_SERVICE_ID') or secrets.token_hex(6)"
if new not in text:
    if old not in text:
        sys.exit('engine.py: the service id line moved; update patch-engine.py')
    text = text.replace(old, new, 1)
    if '\nimport os\n' not in text:
        text = text.replace('\nimport secrets\n', '\nimport os\nimport secrets\n', 1)
    engine.write_text(text, encoding='utf-8')
print('engine patched')
