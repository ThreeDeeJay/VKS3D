import re
from pathlib import Path

wanted = {"Description", "Status", "Installation", "Troubleshooting", "Compatibility", "Roadmap" "Credits"}
text = Path("VKS3D_ReadMe.md").read_text(encoding="utf-8")
lines = text.splitlines()

sections = []
i = 0

while i < len(lines):
    match = re.match(r"^(#{1,6})\s+(.+?)\s*#*\s*$", lines[i])
    if not match:
        i += 1
        continue

    level = len(match.group(1))
    title = match.group(2).strip()
    start = i
    i += 1

    while i < len(lines):
        next_heading = re.match(r"^(#{1,6})\s+", lines[i])
        if next_heading and len(next_heading.group(1)) <= level:
            break
        i += 1

    if title.casefold() in {name.casefold() for name in wanted}:
        sections.append("\n".join(lines[start:i]).strip())

print("\n\n".join(sections))