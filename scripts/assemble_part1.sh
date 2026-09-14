#!/bin/sh
# Assemble Part I (Storage) as one continuous manuscript: front matter + the
# four unit chapters, with a page break between chapters. The cover and TOC
# are added by cover_part1.tex at build time. Run from book/.
set -e
python3 - <<'PY'
import re
BREAK = re.compile(r'^(?:\*\s*){3,}$|^(?:-\s*){3,}$|^(?:_\s*){3,}$')

def strip_fm(path):
    out, in_code, seen_heading = [], False, False
    for line in open(path).read().split('\n'):
        s = line.strip()
        if s.startswith('```'):
            in_code = not in_code; out.append(line); continue
        if not in_code:
            if line.strip() == '# Build a SQL Database Engine in C++': continue
            if line.strip() == '### Through Challenges': continue
            # drop any thematic break that sits before the first heading (it
            # used to separate the title from the body; the title is on the
            # cover now, so the rule would dangle at the top of page 1)
            if not seen_heading and BREAK.match(s): continue
            if line.startswith('## '):        # promote front-matter H2 -> H1
                out.append('#' + line[2:]); seen_heading = True; continue
            if line.startswith('# '): seen_heading = True
        out.append(line)
    return '\n'.join(out).strip() + '\n'

parts = [strip_fm('00-front-matter.md')]
for u in ['01-unit-01-slotted-pages-and-the-pager.md',
          '02-unit-02-btree-insert-and-search.md',
          '03-unit-03-btree-delete-and-range-scans.md',
          '04-unit-04-buffer-pool.md']:
    parts.append('\n```{=latex}\n\\clearpage\n```\n')
    parts.append(open(u).read().strip() + '\n')

open('_part1_body.md','w').write('\n'.join(parts))
print('wrote _part1_body.md')
PY
