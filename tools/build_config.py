"""Write a build configuration stamp only when its contents change."""
import json
from pathlib import Path
import sys


def main():
    path = Path(sys.argv[1])
    content = json.dumps(sys.argv[2:], ensure_ascii=True, indent=2) + "\n"
    if not path.exists() or path.read_text() != content:
        path.write_text(content)


if __name__ == "__main__":
    main()
