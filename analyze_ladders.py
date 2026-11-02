"""
analyze_ladders.py — analysis of every-pair word ladder output.

Usage:
    python analyze_ladders.py ladders_all_shortest.csv common_words_3_to_5.csv [--max-paths 1000] [--out results]

Inputs
    ladders_all_shortest.csv : from `word_ladder --all-pairs` (one row per shortest path)
    common_words_3_to_5.csv  : the dictionary used for in-between words (letter-frequency baseline)

Weighting
    Every connected pair counts equally: each of a pair's listed paths gets weight 1 / (paths listed).
    Analyses are direction-free, because any ladder also works in reverse.

Vowels
    a e i o u are vowels. y counts as a vowel unless it is the first letter or is followed by a vowel
    (so: sky, gym -> vowel; yarn, yes -> consonant).

Outputs (CSV tables in --out)
    1_bridge_letters.csv        letters used in a ladder but absent from both end words, vs dictionary baseline
    2_steps_by_matches.csv      ladder length and detour by number of same-position letters
    2b_shared_anywhere.csv      detour vs letters shared in other positions (anagram-ish overlap)
    3_cv_pattern.csv            detour by consonant/vowel pattern agreement, controlling for length and differing positions
    4a_move_positions.csv       which word position each move changes, split into direct vs detour moves
    4b_move_types.csv           vowel/consonant move types and where in the ladder they happen
    4c_substitutions.csv        most common letter swaps (unordered, e.g. a<->o)
    4d_broken_positions.csv     positions that already match but get temporarily changed
    5_no_ladder_by_letter.csv   share of pairs with no ladder, by letter contained
    summary.txt                 headline numbers
"""
import argparse
import csv
import os
from collections import Counter, defaultdict

import pandas as pd

VOWELS = set("aeiou")
LETTERS = "abcdefghijklmnopqrstuvwxyz"


def is_vowel(word, i):
    c = word[i]
    if c in VOWELS:
        return True
    if c == "y":
        if i == 0:
            return False
        if i + 1 < len(word) and word[i + 1] in VOWELS:
            return False
        return True
    return False


def cv(word):
    return "".join("V" if is_vowel(word, i) else "C" for i in range(len(word)))


def pos_class(i, n):
    return "first" if i == 0 else ("last" if i == n - 1 else "middle")


def shared_anywhere(a, b):
    ca, cb = Counter(a), Counter(b)
    return sum(min(ca[k], cb[k]) for k in ca)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ladders")
    ap.add_argument("dictionary")
    ap.add_argument("--max-paths", type=int, default=1000)
    ap.add_argument("--out", default="results")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    # ---------- dictionary baseline: share of all letter occurrences, by word length ----------
    dict_letters = defaultdict(Counter)
    for row in csv.DictReader(open(args.dictionary)):
        w = row["word"].strip().lower()
        dict_letters[len(w)].update(w)

    # ---------- accumulators ----------
    pairs = []                                   # one record per pair
    bridge_w = defaultdict(float)                # (len, letter) -> weighted pairs using letter as bridge
    bridge_elig = Counter()                      # (len, letter) -> pairs where letter is in neither end word
    bridge_occ = defaultdict(Counter)            # len -> weighted bridge-letter occurrences (letter share)
    move_pos = defaultdict(float)                # (len, pos, kind) -> weight
    move_type = defaultdict(float)               # (len, type, kind) -> weight
    move_centr = defaultdict(lambda: [0.0, 0.0]) # (len, group) -> [weighted centrality sum, weight]
    subs = defaultdict(float)                    # (len, "a<->o", kind) -> weight
    broken = defaultdict(float)                  # (len, pos, vowel/cons) -> weighted pairs breaking a match
    broken_elig = Counter()                      # (len, pos, vowel/cons) -> pairs where that position matches
    word_pairs = Counter()                       # (len, letter) -> pairs containing letter
    word_nolad = Counter()                       # (len, letter) -> those with no ladder

    def finish_pair(rec, paths):
        n = len(rec["start"])
        a, b = rec["start"], rec["end"]
        letters = set(a) | set(b)
        for L in letters:
            word_pairs[(n, L)] += 1
            if rec["steps"] < 0:
                word_nolad[(n, L)] += 1
        pairs.append(rec)
        if rec["steps"] <= 0:
            return
        w = 1.0 / len(paths)
        for L in LETTERS:
            if L not in letters:
                bridge_elig[(n, L)] += 1
        match_pos = [i for i in range(n) if a[i] == b[i]]
        for i in match_pos:
            broken_elig[(n, i, "vowel" if is_vowel(a, i) else "consonant")] += 1

        for path in paths:
            mids = path[1:-1]
            used = set("".join(mids)) - letters
            for L in used:
                bridge_w[(n, L)] += w
            for m in mids:
                for c in m:
                    if c not in letters:
                        bridge_occ[n][c] += w
            changed = set()
            k_total = len(path) - 1
            for k in range(k_total):
                x, y = path[k], path[k + 1]
                i = next(j for j in range(n) if x[j] != y[j])
                changed.add(i)
                direct = (x[i] == a[i] and y[i] == b[i]) or (x[i] == b[i] and y[i] == a[i])
                kind = "direct" if direct else "detour"
                vx, vy = is_vowel(x, i), is_vowel(y, i)
                t = "vowel->vowel" if vx and vy else ("cons->cons" if not vx and not vy else "vowel<->cons")
                # centrality: 0 = first/last move of the ladder, 1 = exact middle (direction-free)
                c = 1.0 if k_total == 1 else min(k, k_total - 1 - k) / ((k_total - 1) / 2)
                move_pos[(n, pos_class(i, n), kind)] += w
                move_type[(n, t, kind)] += w
                for grp in (t, "pos:" + pos_class(i, n), "kind:" + kind):
                    acc = move_centr[(n, grp)]
                    acc[0] += w * c
                    acc[1] += w
                pair_key = "<->".join(sorted((x[i], y[i])))
                subs[(n, pair_key, kind)] += w
            for i in match_pos:
                if i in changed:
                    broken[(n, i, "vowel" if is_vowel(a, i) else "consonant")] += w

    # ---------- stream the ladder file (rows of one pair are contiguous) ----------
    cur, paths = None, []
    with open(args.ladders) as f:
        for row in csv.DictReader(f):
            pid = row["pair_id"]
            if cur is None or pid != cur["pair_id"]:
                if cur is not None:
                    finish_pair(cur, paths)
                a, b = row["start"], row["end"]
                n = len(a)
                ham = sum(a[i] != b[i] for i in range(n))
                steps = int(row["steps"])
                cur = dict(pair_id=pid, start=a, end=b, length=n, steps=steps, hamming=ham,
                           detour=(steps - ham) if steps >= 0 else None,
                           shared_any=shared_anywhere(a, b),
                           cv_a=cv(a), cv_b=cv(b),
                           num_paths=int(row["num_paths"]))
                paths = []
            if row["path"]:
                paths.append(row["path"].split(">"))
        if cur is not None:
            finish_pair(cur, paths)

    P = pd.DataFrame(pairs)
    C = P[P.steps > 0].copy()
    C["matches"] = C.length - C.hamming
    C["extra_shared"] = C.shared_any - C.matches
    C["same_cv"] = C.cv_a == C.cv_b
    C["cv_mismatch"] = [sum(x != y for x, y in zip(p, q)) for p, q in zip(C.cv_a, C.cv_b)]

    # 1. bridge letters
    rows = []
    for n in sorted(dict_letters):
        tot_b = sum(bridge_occ[n].values()) or 1
        tot_d = sum(dict_letters[n].values())
        for L in LETTERS:
            el = bridge_elig[(n, L)]
            share_b = bridge_occ[n][L] / tot_b
            share_d = dict_letters[n][L] / tot_d
            rows.append(dict(length=n, letter=L,
                             pct_ladders_using_as_bridge=100 * bridge_w[(n, L)] / el if el else None,
                             eligible_pairs=el,
                             share_of_bridge_letters=100 * share_b,
                             share_of_dictionary_letters=100 * share_d,
                             enrichment=share_b / share_d if share_d else None))
    t1 = pd.DataFrame(rows)
    allL = t1.groupby("letter").apply(lambda g: pd.Series(dict(
        pct_ladders_using_as_bridge=(g.pct_ladders_using_as_bridge * g.eligible_pairs).sum() / g.eligible_pairs.sum(),
        enrichment=g.enrichment.mean()))).reset_index()
    allL.insert(0, "length", "all")
    t1 = pd.concat([t1, allL]).round(3)
    t1.to_csv(f"{args.out}/1_bridge_letters.csv", index=False)

    # 2. steps by positional matches
    t2 = C.groupby(["length", "matches"]).agg(
        pairs=("steps", "size"), mean_steps=("steps", "mean"), mean_detour=("detour", "mean"),
        pct_with_detour=("detour", lambda s: 100 * (s > 0).mean())).round(3).reset_index()
    t2.to_csv(f"{args.out}/2_steps_by_matches.csv", index=False)
    t2b = C.groupby(["length", "hamming", "extra_shared"]).agg(
        pairs=("steps", "size"), mean_detour=("detour", "mean"),
        pct_with_detour=("detour", lambda s: 100 * (s > 0).mean())).round(3).reset_index()
    t2b = t2b[t2b.pairs >= 30]
    t2b.to_csv(f"{args.out}/2b_shared_anywhere.csv", index=False)

    # 3. CV pattern, controlling for length and hamming distance
    t3 = C.groupby(["length", "hamming", "cv_mismatch"]).agg(
        pairs=("steps", "size"), mean_detour=("detour", "mean"),
        pct_with_detour=("detour", lambda s: 100 * (s > 0).mean())).round(3).reset_index()
    t3 = t3[t3.pairs >= 30]
    t3.to_csv(f"{args.out}/3_cv_pattern.csv", index=False)

    # 4a. positions
    t4a = pd.DataFrame([dict(length=n, position=p, kind=k, weight=v) for (n, p, k), v in move_pos.items()])
    t4a["pct_of_moves"] = 100 * t4a.weight / t4a.groupby("length").weight.transform("sum")
    # moves per position slot (middle has n-2 slots)
    t4a["pct_per_slot"] = t4a.pct_of_moves / t4a.apply(lambda r: r.length - 2 if r.position == "middle" else 1, axis=1)
    t4a.drop(columns="weight").round(3).sort_values(["length", "kind", "position"]).to_csv(
        f"{args.out}/4a_move_positions.csv", index=False)

    # 4b. move types + centrality
    t4b = pd.DataFrame([dict(length=n, move_type=t, kind=k, weight=v) for (n, t, k), v in move_type.items()])
    t4b["pct_of_moves"] = 100 * t4b.weight / t4b.groupby("length").weight.transform("sum")
    cen = pd.DataFrame([dict(length=n, group=g, mean_centrality=s / w if w else None)
                        for (n, g), (s, w) in move_centr.items()])
    t4b.drop(columns="weight").round(3).to_csv(f"{args.out}/4b_move_types.csv", index=False)
    cen.round(3).sort_values(["length", "group"]).to_csv(f"{args.out}/4b_move_centrality.csv", index=False)

    # 4c. substitutions
    t4c = pd.DataFrame([dict(length=n, swap=s, kind=k, weight=v) for (n, s, k), v in subs.items()])
    t4c["pct_of_moves"] = 100 * t4c.weight / t4c.groupby("length").weight.transform("sum")
    t4c = t4c.sort_values(["length", "weight"], ascending=[True, False])
    t4c.drop(columns="weight").round(3).to_csv(f"{args.out}/4c_substitutions.csv", index=False)

    # 4d. broken matching positions
    t4d = pd.DataFrame([dict(length=n, position=i + 1, letter_type=t,
                             pct_pairs_breaking=100 * broken[(n, i, t)] / el, eligible_pairs=el)
                        for (n, i, t), el in broken_elig.items() if el >= 30])
    t4d.round(3).sort_values(["length", "position", "letter_type"]).to_csv(
        f"{args.out}/4d_broken_positions.csv", index=False)

    # 5. no-ladder by letter
    t5 = pd.DataFrame([dict(length=n, letter=L, pairs=c, pct_no_ladder=100 * word_nolad[(n, L)] / c)
                       for (n, L), c in word_pairs.items()]).round(2).sort_values(["length", "letter"])
    t5.to_csv(f"{args.out}/5_no_ladder_by_letter.csv", index=False)

    with open(f"{args.out}/summary.txt", "w") as f:
        for n, g in P.groupby("length"):
            con = g[g.steps > 0]
            f.write(f"{n}-letter: {len(g)} pairs, {100 * (g.steps < 0).mean():.1f}% no ladder, "
                    f"mean steps {con.steps.mean():.2f}, mean detour {con.detour.mean():.2f}, "
                    f"{100 * (con.detour > 0).mean():.1f}% need a detour\n")
    print(open(f"{args.out}/summary.txt").read())


if __name__ == "__main__":
    main()
