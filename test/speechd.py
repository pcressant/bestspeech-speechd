#!/usr/bin/env python3
"""Drive sd_bestspeech through Speech Dispatcher's module protocol.

No server and no sound: the module is started the way the server starts it,
spoken to on its standard input, and what it hands back is checked, down to
the samples, against openbst's own bstspeak for the same text.

The ModuleProcess class is adapted from OpenEVV's test/speechd.py (MIT).
"""

import math
import os
import select
import struct
import subprocess
import sys
import tempfile


class ModuleProcess:
    def __init__(self, binary, config):
        self.process = subprocess.Popen(
            [binary, config],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )

    def send(self, data):
        self.process.stdin.write(data.encode("utf-8"))
        self.process.stdin.flush()

    def read_line(self, timeout=15):
        ready, _, _ = select.select([self.process.stdout], [], [], timeout)
        if not ready:
            raise AssertionError("module timed out")
        line = self.process.stdout.readline()
        if not line:
            error = self.process.stderr.read().decode("utf-8", "replace")
            raise AssertionError(f"module closed unexpectedly; stderr: {error}")
        return line

    def expect(self, wanted):
        line = self.read_line()
        assert line == wanted, (wanted, line)

    def command_with_data(self, command, lines, acknowledgement):
        self.send(command + "\n")
        self.expect(acknowledgement)
        self.send("".join(f"{line}\n" for line in lines) + ".\n")

    def set(self, **values):
        self.command_with_data(
            "SET",
            [f"{name}={value}" for name, value in values.items()],
            b"203 OK RECEIVING SETTINGS\n",
        )
        self.expect(b"203 OK SETTINGS RECEIVED\n")

    def list_voices(self, language=None):
        command = "LIST VOICES" + (" " + language if language else "")
        self.send(command + "\n")
        voices = []
        while True:
            line = self.read_line()
            if line == b"200 OK VOICE LIST SENT\n":
                return voices
            voices.append(line.decode("utf-8").removeprefix("200-").strip().split("\t"))

    def speak(self, text, command="SPEAK", interrupt=None):
        self.icons = []
        self.metadata = {}
        self.command_with_data(command, [text], b"202 OK RECEIVING MESSAGE\n")
        self.expect(b"200 OK SPEAKING\n")
        self.expect(b"701 BEGIN\n")
        audio = bytearray()
        marks = []
        interrupted = False
        while True:
            line = self.read_line(30)
            if line.startswith(b"705-") and not line.startswith(b"705-AUDIO"):
                key, value = line[4:].decode("ascii").strip().split("=", 1)
                self.metadata[key] = int(value)
            elif line.startswith(b"705-AUDIO\0"):
                payload = line[len(b"705-AUDIO\0"):-1]
                at = 0
                while at < len(payload):
                    if payload[at] == 0x7D:
                        at += 1
                        audio.append(payload[at] ^ 0x20)
                    else:
                        audio.append(payload[at])
                    at += 1
                self.expect(b"705 AUDIO\n")
                if interrupt and not interrupted:
                    self.send(interrupt + "\n")
                    interrupted = True
            elif line.startswith(b"700-"):
                marks.append(line[4:].decode("utf-8").strip())
                self.expect(b"700 INDEX MARK\n")
            elif line.startswith(b"706-"):
                self.icons.append(line[4:].decode("utf-8").strip())
                self.expect(b"706 ICON\n")
            elif line in (b"702 END\n", b"703 STOP\n", b"704 PAUSE\n"):
                return bytes(audio), marks, line.strip().decode("ascii")
            else:
                raise AssertionError(f"unexpected module response: {line!r}")

    def close(self):
        if self.process.poll() is None:
            self.send("QUIT\n")
            self.expect(b"210 OK QUIT\n")
        return self.process.wait(timeout=10)


def direct(bstspeak, build, text, codeset):
    """What openbst itself says for the text, as raw samples."""
    return subprocess.check_output(
        [bstspeak, "--build", build, "--raw", text.encode(codeset)]
    )


def samples(audio):
    return struct.unpack(f"<{len(audio) // 2}h", audio)


def peak(audio):
    return max(abs(s) for s in samples(audio))


def voiced_rms(audio):
    voiced = [s for s in samples(audio) if abs(s) > 100]
    return math.sqrt(sum(s * s for s in voiced) / len(voiced))


def edges(audio, rate):
    """Silence before and after the speech, in milliseconds."""
    s = samples(audio)
    lead = next(i for i, x in enumerate(s) if abs(x) > 8)
    trail = next(i for i, x in enumerate(reversed(s)) if abs(x) > 8)
    return 1000 * lead / rate, 1000 * trail / rate


def longest_gap(audio, rate):
    """The longest run of near silence inside the speech, in milliseconds."""
    s = samples(audio)
    lead = next(i for i, x in enumerate(s) if abs(x) > 8)
    end = len(s) - next(i for i, x in enumerate(reversed(s)) if abs(x) > 8)
    longest = run = 0
    for x in s[lead:end]:
        run = run + 1 if abs(x) <= 8 else 0
        longest = max(longest, run)
    return 1000 * longest / rate


def step(name):
    print(f"speechd: {name}", flush=True)


def start(binary, config):
    module = ModuleProcess(binary, config)
    module.send("INIT\n")
    module.expect(b"299-BestSpeech initialized\n")
    module.expect(b"299 OK LOADED SUCCESSFULLY\n")
    module.command_with_data("AUDIO", ["audio_output_method=server"],
                             b"207 OK RECEIVING AUDIO SETTINGS\n")
    module.expect(b"203 OK AUDIO INITIALIZED\n")
    module.set(rate="0", pitch="0", pitch_range="0", volume="100",
               punctuation_mode="none", spelling_mode="off",
               cap_let_recogn="none", voice="male1", language="en",
               synthesis_voice="NULL")
    return module


ORCA = ('<speak><mark name="0:7"/>Bonjour <mark name="8:2"/>le '
        '<mark name="11:5"/>monde <mark name="17:3"/>est <mark name="21:5"/>'
        'grand.</speak>')
ORCA_TEXT = "Bonjour le monde est grand."
ORCA_MARKS = ["0:7", "8:2", "11:5", "17:3", "21:5"]


def raw_checks(module, bstspeak):
    """The engine's samples untouched: compared with bstspeak."""
    step("list voices")
    voices = module.list_voices()
    # Five builds, fourteen presets each, French and English only.
    assert len(voices) == 70, len(voices)
    assert {v[1] for v in voices} == {"fr", "en"}, {v[1] for v in voices}
    names = {v[0] for v in voices}
    for wanted in ("fr-2006-Fred", "fr-1998-Sara", "en-1995-Fred",
                   "en-1998-Granny", "en-2006-Kit"):
        assert wanted in names, wanted

    step("English, 1995, identical to bstspeak")
    audio, marks, event = module.speak("Keynote’s quick brown fox jumps over the lazy dog")
    assert event == "702 END" and marks == []
    wanted = {"bits": 16, "num_channels": 1, "sample_rate": 11025, "big_endian": 0}
    assert all(module.metadata.get(k) == v for k, v in wanted.items()), module.metadata
    assert audio == direct(bstspeak, "1995",
                           "Keynote's quick brown fox jumps over the lazy dog", "ascii")

    step("French, 2006, accents, identical to bstspeak")
    module.set(language="fr")
    text = "Élève où est le garçon"
    audio, _, event = module.speak(text)
    assert event == "702 END"
    assert module.metadata["sample_rate"] == 10000
    assert audio == direct(bstspeak, "2006FRE", text, "cp1252")
    assert audio != direct(bstspeak, "2006FRE", "Eleve ou est le garcon", "ascii")

    step("a screen reader's mark before every word: one sentence, one synthesis")
    audio, marks, event = module.speak(ORCA)
    assert event == "702 END" and marks == ORCA_MARKS, marks
    assert audio == direct(bstspeak, "2006FRE", ORCA_TEXT, "ascii")

    step("2006: every c spelled out, since the build reads them all as s")
    audio, _, _ = module.speak("Comme aucun garçon, avec ceci.")
    assert audio == direct(bstspeak, "2006FRE", "Komme aukun garson: avek sessi.", "cp1252")
    assert audio != direct(bstspeak, "2006FRE", "Comme aucun garçon: avec ceci.", "cp1252")
    audio, _, _ = module.speak("c", command="CHAR")
    assert audio == direct(bstspeak, "2006FRE", "sé ", "cp1252")
    audio, _, _ = module.speak("100 pour cent")
    assert audio == direct(bstspeak, "2006FRE", "100 pour sant", "ascii")
    audio, _, _ = module.speak("après la pluie")
    assert audio == direct(bstspeak, "2006FRE", "aprè la pluie", "cp1252")

    step("2006: a comma is a phrase break, not the end of the text")
    assert direct(bstspeak, "2006FRE", "Bonjour, au revoir.", "ascii") == \
        direct(bstspeak, "2006FRE", "Bonjour,", "ascii")
    audio, _, _ = module.speak("Bonjour, au revoir.")
    assert audio == direct(bstspeak, "2006FRE", "Bonjour: au revoir.", "ascii")

    step("numbers that silence the engine")
    for text, engine in (
        ("Il est 10 30 et demie", "Il est 10  30 et demie"),
        ("1 2 3", "1  2  3"),
        ("1 000 euros", "1000 euros"),
        ("1 000 euros", "1000 euros"),
        ("3,5 euros", "3 virgule 5 euros"),
        ("1 ,5 euros", "1  virgule 5 euros"),
        ("Il est 10:30", "Il est 10  30"),
    ):
        audio, _, _ = module.speak(text)
        assert audio and audio == direct(bstspeak, "2006FRE", engine, "ascii"), text
    module.set(synthesis_voice="en-1995-Fred")
    audio, _, _ = module.speak("1,000 dollars and 10 30")
    assert audio == direct(bstspeak, "1995", "1000 dollars and 10  30", "ascii")
    module.set(synthesis_voice="NULL")

    step("French, 1998, read in the DOS set")
    module.set(synthesis_voice="fr-1998-Fred")
    text = "Élève, où est le garçon"
    audio, _, event = module.speak(text)
    assert event == "702 END" and audio
    assert module.metadata["sample_rate"] == 11025
    assert audio == direct(bstspeak, "1998FRN", text, "cp850")

    step("English 1998 and 2006 by voice name")
    for voice, build in (("en-1998-Fred", "1998ENG"), ("en-2006-Fred", "2006ENG")):
        module.set(synthesis_voice=voice)
        audio, _, event = module.speak("Good morning")
        assert event == "702 END"
        assert audio == direct(bstspeak, build, "Good morning", "ascii"), voice

    step("a character the set has not got is not read as a question mark")
    module.set(synthesis_voice="NULL", language="fr")
    audio, _, event = module.speak("prix \u2603 fixe")
    assert event == "702 END"
    assert audio != direct(bstspeak, "2006FRE", "prix ? fixe", "ascii")

    step("rate")
    module.set(language="en")
    lengths = {}
    for rate in (-100, -50, 0, 50, 100):
        module.set(rate=str(rate))
        audio, _, event = module.speak("The quick brown fox jumps over the lazy dog")
        assert event == "702 END"
        lengths[rate] = len(audio)
    assert lengths[-100] > lengths[-50] > lengths[0] > lengths[50] > lengths[100], lengths
    assert 0.18 < lengths[100] / lengths[0] < 0.32, lengths
    assert 3.0 < lengths[-100] / lengths[0] < 4.6, lengths
    module.set(rate="0")

    step("pitch, range, presets")
    base = direct(bstspeak, "1995", "The quick brown fox", "ascii")
    module.set(pitch="60")
    assert module.speak("The quick brown fox")[0] != base
    module.set(pitch="0", pitch_range="60")
    assert module.speak("The quick brown fox")[0] != base
    module.set(pitch_range="0", voice="female1")
    sara = module.speak("The quick brown fox")[0]
    module.set(voice="male1", synthesis_voice="en-1995-Sara")
    assert module.speak("The quick brown fox")[0] == sara != base
    module.set(synthesis_voice="en-1995-Granny")
    granny = module.speak("The quick brown fox")[0]
    assert granny not in (sara, base)
    module.set(synthesis_voice="NULL")

    step("characters, keys, icons, capitals")
    audio, _, event = module.speak("a", command="CHAR")
    assert event == "702 END" and audio
    audio, _, event = module.speak("KP_Enter", command="KEY")
    assert event == "702 END" and audio
    audio, _, event = module.speak("dialog-warning", command="SOUND_ICON")
    assert event == "702 END" and module.icons == ["dialog-warning"]
    module.set(cap_let_recogn="spell")
    spoken, _, _ = module.speak("A", command="CHAR")
    module.set(cap_let_recogn="none")
    plain, _, _ = module.speak("A", command="CHAR")
    assert len(spoken) > len(plain)
    module.set(cap_let_recogn="icon")
    audio, _, _ = module.speak("A", command="CHAR")
    assert module.icons == ["capital"] and audio
    module.set(cap_let_recogn="none")

    step("punctuation none drops what the server already named; a tilde is no command")
    dropped, _, _ = module.speak("hello # world")
    module.set(punctuation_mode="all")
    kept, _, _ = module.speak("hello # world")
    module.set(punctuation_mode="none")
    assert len(dropped) < len(kept)
    assert module.speak("one ~| two.")[0] == module.speak("one    two.")[0]


def processed_checks(module):
    """What a listener gets: level, silences, flow, interruption."""
    step("loudness at the default volume")
    for voice in ("fr-2006-Fred", "fr-1998-Fred", "en-1995-Fred", "en-2006-Fred"):
        module.set(synthesis_voice=voice)
        audio, _, _ = module.speak("Le chat est sur le tapis et il regarde la fenêtre.")
        level = voiced_rms(audio)
        assert 3500 < level < 8000 and peak(audio) <= 32767, (voice, level)
    module.set(synthesis_voice="fr-2006-Fred", volume="0")
    half = voiced_rms(module.speak("Le chat est sur le tapis.")[0])
    module.set(volume="100")
    full = voiced_rms(module.speak("Le chat est sur le tapis.")[0])
    assert 0.4 < half / full < 0.65, (half, full)

    step("no silence before the speech, almost none after it")
    for voice, rate in (("fr-2006-Fred", 10000), ("en-1995-Fred", 11025)):
        module.set(synthesis_voice=voice)
        for text in ("Fichier", "Ouvrir le menu.", "a"):
            audio, _, _ = module.speak(text, command="CHAR" if text == "a" else "SPEAK")
            lead, trail = edges(audio, rate)
            assert lead < 2 and trail < 25, (voice, text, lead, trail)

    step("a mark before every word does not cut the sentence up")
    module.set(synthesis_voice="fr-2006-Fred")
    audio, marks, event = module.speak(ORCA)
    assert event == "702 END" and marks == ORCA_MARKS
    assert longest_gap(audio, 10000) < 120, longest_gap(audio, 10000)

    step("sentences follow each other with a short pause")
    audio, _, _ = module.speak("Premier. Deuxième. Troisième.")
    assert longest_gap(audio, 10000) < 260, longest_gap(audio, 10000)

    step("numbers are heard")
    for text in ("Il est 10 30", "1 2 3", "06 12 34 56 78", "3,5"):
        audio, _, _ = module.speak(text)
        assert audio and peak(audio) > 1000, text

    step("stop")
    long_text = " ".join(["This sentence keeps going so that there is something to stop."] * 20)
    module.set(synthesis_voice="en-1995-Fred")
    full = len(module.speak(long_text)[0])
    audio, _, event = module.speak(long_text, interrupt="STOP")
    assert event == "703 STOP" and len(audio) < full // 4, (len(audio), full)

    step("pause at the next index mark")
    pause_text = ('<speak>First part of the text, long enough to pause in.'
                  '<mark name="__spd_1"/> Second part.</speak>')
    audio, marks, event = module.speak(pause_text, interrupt="PAUSE")
    assert event == "704 PAUSE" and audio and marks == ["__spd_1"], (event, marks)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: speechd.py <sd_bestspeech> <bestspeech.conf> <bstspeak>")
    binary, config, bstspeak = (os.path.abspath(a) for a in sys.argv[1:])
    with tempfile.TemporaryDirectory() as work:
        raw_config = os.path.join(work, "raw.conf")
        with open(config) as source, open(raw_config, "w") as raw:
            raw.write(source.read() + "\nRawOutput 1\n")
        for conf, checks in ((raw_config, lambda m: raw_checks(m, bstspeak)),
                             (config, processed_checks)):
            module = start(binary, conf)
            try:
                checks(module)
                assert module.close() == 0
            finally:
                if module.process.poll() is None:
                    module.process.kill()
    print("speechd: all checks passed")


if __name__ == "__main__":
    main()
