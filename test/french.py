"""The French respelling, word by word: what build/frspell makes of each."""
import subprocess
import sys

CASES = {
    # Hard c is k, soft c s, or ss between vowels.
    "comme": "komme", "aucun": "aukun", "école": "ékole", "Car": "kar",
    "garçon": "garson", "leçon": "lesson", "place": "plasse",
    "France": "Franse", "Ceci": "sessi", "cuisine": "kuisine",
    # Pairs and clusters.
    "accès": "aksès", "accord": "akord", "acquérir": "aquérir",
    "science": "siense", "descendre": "dessendre", "second": "segond",
    # ch: sh, but k before a consonant and in words from Greek.
    "chat": "chat", "chrome": "krome", "technique": "teknique",
    "psychologie": "psykologie", "orchestre": "orkestre", "écho": "éko",
    "marchand": "marchand", "déchet": "dhéchet",
    # Word ends.
    "avec": "avek", "donc": "donk", "blanc": "blan", "ajonc": "ajon",
    "tabac": "taba", "direct": "direktt", "respect": "respet",
    "actrice": "aktrysse",
    # The letter alone.
    "c": "sé", "ç": "sé sédille",
    # Après takes its z before a vowel only.
    "après la pluie": "aprè la pluie", "après un jour": "après un jour",
    "après.": "aprè.", "après-midi": "aprè-midi",
}


def main():
    words = list(CASES)
    out = subprocess.run([sys.argv[1]], input="".join(w + "\n" for w in words),
                         capture_output=True, text=True, check=True).stdout
    got = out.split("\n")
    wrong = [(w, g, CASES[w]) for w, g in zip(words, got) if g != CASES[w]]
    for w, g, want in wrong:
        print(f"french: {w!r} gave {g!r}, wanted {want!r}")
    if wrong:
        raise SystemExit(1)
    print(f"french: {len(words)} respellings right")


if __name__ == "__main__":
    main()
