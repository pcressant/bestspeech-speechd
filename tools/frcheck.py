"""Checks the French builds against espeak-ng, word by word.

usage: frcheck.py BUILD-DIR WORDS [N]

WORDS is one word to a line, the Hunspell French dictionary for instance.
Each word goes to the 2006 build as the module respells it, to the 2006
build as it stands and to the 1998 build; the sounds each makes (read out of
the engine by phdump) are compared with espeak-ng's. Open and closed e, o and
eu are taken as one, and schwa is left out: those are where French speakers
differ too. Prints how many words disagree and the N commonest differences.
"""
import collections
import difflib
import os
import re
import subprocess
import sys

CODES = {
    "01": "p", "02": "t", "03": "k", "04": "b", "05": "d", "06": "g", "07": "m",
    "08": "n", "09": "ɲ", "0a": "ŋ", "0b": "f", "0c": "s", "0d": "ʃ", "0e": "v",
    "0f": "z", "10": "ʒ", "11": "l", "12": "ʁ", "14": "j", "15": "ɥ", "16": "w",
    "17": "i", "18": "y", "19": "E", "1a": "Œ", "1b": "E", "1c": "Œ", "1d": "A",
    "1e": "A", "1f": "O", "20": "O", "21": "u", "22": "Ẽ", "23": "Ẽ", "24": "Ã",
    "25": "Õ",
}
SILENT = {"2f", "26"}  # pause, schwa
IPA = [("ɑ̃", "Ã"), ("ɛ̃", "Ẽ"), ("œ̃", "Ẽ"), ("ɔ̃", "Õ"), ("ɛ", "E"), ("e", "E"),
       ("ø", "Œ"), ("œ", "Œ"), ("ɑ", "A"), ("a", "A"), ("ɔ", "O"), ("o", "O"),
       ("ɡ", "g"), ("r", "ʁ")]


def engine_sounds(line):
    codes = line.split("\t", 1)[1] if "\t" in line else ""
    out = [CODES.get(c, "?" + c) for c in codes.replace("|", " ").split()
           if c not in SILENT]
    return out[2:] if out[:2] == ["ɲ", "b"] else out  # the phrase header


def espeak_sounds(ipa):
    ipa = re.sub(r"\([a-z]+\)", "", ipa)
    ipa = "".join(ch for ch in ipa if ch not in "ˈˌː- ")
    out, i = [], 0
    while i < len(ipa):
        for a, b in IPA:
            if ipa.startswith(a, i):
                out.append(b)
                i += len(a)
                break
        else:
            if ipa[i] != "ə":
                out.append(ipa[i])
            i += 1
    return out


def run(cmd, text):
    return subprocess.run(cmd, input=text, capture_output=True, check=True).stdout


def check(build_dir, build, words, respell, reference, show):
    codeset = "cp1252" if build.startswith("2006") else "cp850"
    keep = [i for i, w in enumerate(words) if w.encode(codeset, "ignore").decode(codeset) == w]
    words = [words[i] for i in keep]
    reference = [reference[i] for i in keep]
    text = "".join(w + "\n" for w in words)
    if respell:
        text = run([os.path.join(build_dir, "frspell")], text.encode()).decode()
    lines = run([os.path.join(build_dir, "phdump"), build],
                text.encode(codeset, "replace")).decode(codeset).split("\n")
    bad, diffs = 0, collections.defaultdict(list)
    for word, line, ipa in zip(words, lines, reference):
        a, b = espeak_sounds(ipa), engine_sounds(line)
        if a == b:
            continue
        bad += 1
        for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
            if op != "equal":
                diffs[("".join(a[i1:i2]) or "∅") + "→" + ("".join(b[j1:j2]) or "∅")].append(word)
    name = build + (" respelled" if respell else " as it stands")
    print(f"{name}: {bad} of {len(words)} words differ from espeak-ng ({100 * bad / len(words):.1f} %)")
    for key, found in sorted(diffs.items(), key=lambda kv: -len(kv[1]))[:show]:
        print(f"  {len(found):6d}  {key:10s} {' '.join(found[:6])}")


def main():
    build_dir, path = sys.argv[1], sys.argv[2]
    show = int(sys.argv[3]) if len(sys.argv) > 3 else 15
    words = sorted({w.split("/")[0].strip() for w in open(path, encoding="utf-8")
                    if re.fullmatch(r"[a-zàâçéèêëîïôùûüÿœæ'-]+", w.split("/")[0].strip())})
    reference = run(["espeak-ng", "-v", "fr", "-q", "--ipa"],
                    "".join(w + ".\n" for w in words).encode()).decode().split("\n")
    for build, respell in (("2006FRE", True), ("2006FRE", False), ("1998FRN", False)):
        check(build_dir, build, words, respell, reference, show if respell else 5)


if __name__ == "__main__":
    main()
