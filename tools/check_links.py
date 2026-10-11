#!/usr/bin/env python3
"""Check every relative link and #anchor in the repository's Markdown files.

Usage: python3 tools/check_links.py [--external]
Exit status 1 if any internal link or anchor is broken. External (http) links are only listed unless --external is given
(this sandbox cannot reach most hosts, so they are not fetched).
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
LINK = re.compile(r'(?<!\!)\[[^\]]*\]\(([^)\s]+)(?:\s+"[^"]*")?\)')
FENCE = re.compile(r'^(```|~~~)')

def slug(heading: str) -> str:
    """GitHub-style anchor: lowercase, strip markup, drop punctuation except hyphens/underscores, spaces -> hyphens."""
    h = re.sub(r'`([^`]*)`', r'\1', heading)
    h = re.sub(r'<[^>]+>', '', h)
    h = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', h)
    h = h.strip().lower()
    h = re.sub(r'[^\w\- ]', '', h, flags=re.UNICODE)
    return h.replace(' ', '-')

def anchors(path: pathlib.Path):
    seen, out, fence = {}, set(), False
    for line in path.read_text(encoding='utf-8').splitlines():
        if FENCE.match(line.strip()): fence = not fence; continue
        if fence: continue
        m = re.match(r'^(#{1,6})\s+(.*?)\s*#*\s*$', line)
        if m:
            s = slug(m.group(2)); n = seen.get(s, 0); seen[s] = n + 1
            out.add(s if n == 0 else f'{s}-{n}')
    return out

def main():
    bad, external, checked = [], set(), 0
    files = sorted(p for p in ROOT.rglob('*.md') if '.git' not in p.parts)
    cache = {}
    for md in files:
        fence = False
        for ln, line in enumerate(md.read_text(encoding='utf-8').splitlines(), 1):
            if FENCE.match(line.strip()): fence = not fence; continue
            if fence: continue
            line_nocode = re.sub(r'`[^`]*`', '', line)
            for target in LINK.findall(line_nocode):
                if re.match(r'^(https?:|mailto:)', target): external.add(target); continue
                checked += 1
                path_part, _, frag = target.partition('#')
                dest = (md.parent / path_part).resolve() if path_part else md
                if not dest.exists():
                    bad.append(f'{md.relative_to(ROOT)}:{ln}: missing file  {target}'); continue
                if frag and dest.suffix == '.md':
                    if dest not in cache: cache[dest] = anchors(dest)
                    if frag.lower() not in cache[dest]:
                        bad.append(f'{md.relative_to(ROOT)}:{ln}: missing anchor  {target}')
    print(f'{len(files)} markdown files, {checked} internal links checked, {len(external)} distinct external links (not fetched)')
    for b in bad: print('BROKEN', b)
    if '--external' in sys.argv:
        for e in sorted(external): print('external', e)
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
