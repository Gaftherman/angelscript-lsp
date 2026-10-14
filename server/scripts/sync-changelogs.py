import re

def sync():
    with open('CHANGELOG.md', 'r', encoding='utf-8') as f:
        root_text = f.read()

    match = re.search(r'^## \[', root_text, re.MULTILINE)
    if not match:
        raise ValueError("Could not find any ## [version] in CHANGELOG.md")

    body = root_text[match.start():]
    client_header = '# Change Log\n\nAll notable changes to the "angelscript" extension will be documented in this file.\n\nCheck [Keep a Changelog](http://keepachangelog.com/) for recommendations on how to structure this file.\n\n'

    with open('client/CHANGELOG.md', 'w', encoding='utf-8', newline='\n') as f:
        f.write(client_header + body)

    print("client/CHANGELOG.md successfully synchronized with CHANGELOG.md")

if __name__ == '__main__':
    sync()
