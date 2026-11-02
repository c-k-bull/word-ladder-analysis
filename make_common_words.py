"""
Rebuilds common_words_3_to_5.csv: common 3-5 letter words from the ENABLE dictionary.

Requires:  python3 -m pip install wordfreq
Run:       python3 make_common_words.py

Offensive words (profanity and slurs) are removed. Each blocked word is stored only as the 
first 16 hex characters of its SHA-256 hash, and words are checked by hashing them.
"""
import csv
import hashlib
import urllib.request

from wordfreq import zipf_frequency

ENABLE_URL = "https://raw.githubusercontent.com/dolph/dictionary/master/enable1.txt"
MIN_ZIPF = 3.0   # about once per million words of everyday English

BLOCKED_HASHES = {
    "0033728f0fbc83a0", "0be9b885f4a35a18", "0dcc4d3364433181", "0f28c4960d96647e",
    "11cf8376a1575fdc", "13a465fc6616da8d", "158869a97379229b", "1600725e92dc8cb0",
    "16ea09fc78ca83ca", "17bde8b4064612e2", "2189c0ed714f0c54", "28f8258878dcf516",
    "2a6937d721b14cdd", "2da888fb5e7c5a6f", "4140197ec7959b1c", "45fb7c3b72b6856c",
    "517ae3f73216da64", "522788b65f01ccc0", "566f532d486c9477", "59cc3cb8a4297301",
    "619bc82e0d200e4a", "6556c1a58444e10f", "6ac3c336e4094835", "6e7727239c684e8b",
    "70aafb4266950c0d", "737dd1bca21d67a7", "78a00d189cb84bce", "7902e27065cfa96f",
    "796e43a5a8cdb73b", "7bc671151cbfaee7", "7e85c676fd97d370", "8171c4ecae07283c",
    "83621b34ec5955e5", "85fc17f7069acd39", "867d78976655050c", "886d51e97ad7931d",
    "89133bc00e9b3eb8", "8c5c04391361cbf4", "8f5083e3e5c7dc89", "92a9bf818e7e0002",
    "937d56e49744d206", "94a2b576a9f577e9", "98b52c4b6b7d1f48", "9915ba2d822280f2",
    "99e367bcad7a502f", "9ae315a94e428a7e", "ad505b0be8a49b89", "b04a8a4006358447",
    "bbf8bb7863176009", "c2c3b68b48832afd", "c3de533e9b7fe63b", "cc02032349c833ac",
    "ccff74c2d57f45d2", "d75a838dc758ba17", "daec0c235d3135fc", "dd92623b0a4b255f",
    "e14a747f150b845a", "e7b98c6aa5b944e0", "f6952d6eef555ddd", "f9d0d9b18ae9033a",
    "fa1597ae5dc4c56d", "fa38a8d3e5b92744"
}


def is_blocked(word):
    return hashlib.sha256(word.encode()).hexdigest()[:16] in BLOCKED_HASHES


words = set(urllib.request.urlopen(ENABLE_URL).read().decode().split())

rows = [(w, len(w), round(zipf_frequency(w, "en"), 2)) for w in words
        if 3 <= len(w) <= 5 and w.isalpha() and not is_blocked(w)
        and zipf_frequency(w, "en") >= MIN_ZIPF]
rows.sort(key=lambda r: (r[1], -r[2], r[0]))

with open("common_words_3_to_5.csv", "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["word", "length", "zipf_frequency"])
    writer.writerows(rows)
print(len(rows), "words written")
