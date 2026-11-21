"""Create local credentials once; never overwrite an existing env file."""
from pathlib import Path
import argparse
import secrets

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--env', default='server/.env', help='Target file, relative to the project root.')
parser.add_argument('--template', default='server/.env.example')
args = parser.parse_args()
target = root / args.env
if target.exists():
    print(f'{args.env} already exists; preserved.')
else:
    template = (root / args.template).read_text(encoding='utf-8')
    template = template.replace('DEVICE_TOKEN=\n', 'DEVICE_TOKEN=' + secrets.token_urlsafe(32) + '\n')
    template = template.replace('ADMIN_PASSWORD=\n', 'ADMIN_PASSWORD=' + secrets.token_urlsafe(18) + '\n')
    with target.open('x', encoding='utf-8') as f:
        f.write(template)
    print(f'Created {args.env}. Open it locally to read the login credentials; do not share this file.')
