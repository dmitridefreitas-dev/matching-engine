"""Fetch the LOBSTER AMZN 2012-06-21 sample message file into data/.

LOBSTER (lobsterdata.com) publishes free sample files; their site now
serves them behind a JS app, so this pulls a mirrored copy of the same
sample (kpetridis24/lobsim, stored in Git LFS). ~11 MB, 269,748 messages,
one full trading day, level-10 feed. data/ is gitignored — run this once
before `lob_bench --flow lobster`.
"""

from pathlib import Path
from urllib.request import urlretrieve

URL = (
    "https://media.githubusercontent.com/media/kpetridis24/lobsim/master/"
    "sample_data/AMZN_2012-06-21_34200000_57600000_message_10.csv"
)
DEST = Path(__file__).resolve().parent.parent / "data" / "AMZN_message.csv"


def main() -> None:
    DEST.parent.mkdir(exist_ok=True)
    if DEST.exists():
        print(f"already present: {DEST}")
        return
    print(f"downloading {URL}\n         -> {DEST}")
    urlretrieve(URL, DEST)
    lines = sum(1 for _ in open(DEST, encoding="utf-8"))
    print(f"done: {lines:,} messages")


if __name__ == "__main__":
    main()
