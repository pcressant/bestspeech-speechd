#!/usr/bin/env python3
"""Drive sd_bestspeech through Speech Dispatcher's module protocol.

No server and no sound: the module is started the way the server starts it,
spoken to on its standard input, and what it hands back is checked, down to
the samples, against openbst's own bstspeak for the same text.

The ModuleProcess class is adapted from OpenEVV's test/speechd.py (MIT).
"""

import os
import select
import struct
import subprocess
import sys


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


def peak(audio):
    return max(abs(s) for s in struct.unpack(f"<{len(audio) // 2}h", audio))


def step(name):
    print(f"speechd: {name}", flush=True)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: speechd.py <sd_bestspeech> <bestspeech.conf> <bstspeak>")
    binary, config, bstspeak = (os.path.abspath(a) for a in sys.argv[1:])
    module = ModuleProcess(binary, config)
    try:
        step("initialise")
        module.send("INIT\n")
        module.expect(b"299-BestSpeech initialized\n")
        module.expect(b"299 OK LOADED SUCCESSFULLY\n")

        step("list voices")
        voices = module.list_voices()
        # Five builds, fourteen presets each, French and English only.
        assert len(voices) == 70, len(voices)
        assert {v[1] for v in voices} == {"fr", "en"}, {v[1] for v in voices}
        names = {v[0] for v in voices}
        for wanted in ("fr-2006-Fred", "fr-1998-Sara", "en-1995-Fred",
                       "en-1998-Granny", "en-2006-Kit"):
            assert wanted in names, wanted

        step("audio and settings")
        module.command_with_data("AUDIO", ["audio_output_method=server"],
                                 b"207 OK RECEIVING AUDIO SETTINGS\n")
        module.expect(b"203 OK AUDIO INITIALIZED\n")
        module.set(rate="0", pitch="0", pitch_range="0", volume="0",
                   punctuation_mode="none", spelling_mode="off",
                   cap_let_recogn="none", voice="male1", language="en",
                   synthesis_voice="NULL")

        step("English, 1995, identical to bstspeak")
        audio, marks, event = module.speak("Keynote’s quick brown fox jumps over the lazy dog")
        assert event == "702 END" and marks == []
        wanted = {"bits": 16, "num_channels": 1, "sample_rate": 11025, "big_endian": 0}
        assert all(module.metadata.get(k) == v for k, v in wanted.items()), module.metadata
        reference = direct(bstspeak, "1995",
                           "Keynote's quick brown fox jumps over the lazy dog", "ascii")
        assert audio == reference, (len(audio), len(reference))
        english = audio

        step("French, 2006, accents, identical to bstspeak")
        module.set(language="fr")
        text = "Élève où est le garçon"
        audio, _, event = module.speak(text)
        assert event == "702 END"
        assert module.metadata["sample_rate"] == 10000
        assert audio == direct(bstspeak, "2006FRE", text, "cp1252")
        unaccented = direct(bstspeak, "2006FRE", "Eleve ou est le garcon", "ascii")
        assert audio != unaccented

        step("2006: nothing is lost after a comma")
        # Given the whole text the 2006 builds stop at the first comma; the
        # module says it a phrase at a time.
        cut = direct(bstspeak, "2006FRE", "Bonjour, au revoir.", "ascii")
        assert cut == direct(bstspeak, "2006FRE", "Bonjour,", "ascii")
        audio, _, event = module.speak("Bonjour, au revoir.")
        assert event == "702 END"
        assert audio == (direct(bstspeak, "2006FRE", "Bonjour,", "ascii")
                         + direct(bstspeak, "2006FRE", " au revoir.", "ascii"))
        audio, _, _ = module.speak("1,5 euros")
        assert audio == direct(bstspeak, "2006FRE", "1 virgule 5 euros", "ascii")
        # As Speech Dispatcher's symbol pass hands it over.
        audio, _, _ = module.speak("1 ,5 euros")
        assert audio == direct(bstspeak, "2006FRE", "1  virgule 5 euros", "ascii")
        module.set(synthesis_voice="en-2006-Fred")
        audio, _, _ = module.speak("1,000 dollars, then more")
        assert audio == (direct(bstspeak, "2006ENG", "1000 dollars,", "ascii")
                         + direct(bstspeak, "2006ENG", " then more", "ascii"))
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
        audio, _, event = module.speak("prix ☃ fixe")
        assert event == "702 END"
        assert audio != direct(bstspeak, "2006FRE", "prix ? fixe", "ascii")

        step("several sentences")
        module.set(language="en")
        audio, _, event = module.speak("One. Two! Three?\nFour")
        assert event == "702 END" and len(audio) > len(english) // 4

        step("rate")
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

        step("pitch, range, volume")
        module.set(pitch="60")
        audio, _, _ = module.speak("The quick brown fox")
        base = direct(bstspeak, "1995", "The quick brown fox", "ascii")
        assert audio != base
        module.set(pitch="0", pitch_range="60")
        audio, _, _ = module.speak("The quick brown fox")
        assert audio != base
        module.set(pitch_range="0", volume="-60")
        audio, _, _ = module.speak("The quick brown fox")
        assert len(audio) == len(base) and peak(audio) < peak(base) * 0.5
        module.set(volume="0")

        step("voice types and presets")
        module.set(voice="female1")
        sara, _, _ = module.speak("The quick brown fox")
        module.set(voice="male1", synthesis_voice="en-1995-Sara")
        named, _, _ = module.speak("The quick brown fox")
        assert sara == named and sara != base
        module.set(synthesis_voice="en-1995-Granny")
        granny, _, _ = module.speak("The quick brown fox")
        assert granny != sara and granny != base
        module.set(synthesis_voice="fr-2006-Sara", language="fr")
        frSara, _, _ = module.speak("Bonjour")
        assert frSara != direct(bstspeak, "2006FRE", "Bonjour", "ascii")
        module.set(synthesis_voice="NULL", language="en")

        step("index marks")
        audio, marks, event = module.speak(
            '<speak>Hello <mark name="one"/>world<mark name="two"/></speak>')
        assert event == "702 END" and marks == ["one", "two"] and audio

        step("characters, keys, icons")
        audio, _, event = module.speak("a", command="CHAR")
        assert event == "702 END" and audio
        audio, _, event = module.speak("KP_Enter", command="KEY")
        assert event == "702 END" and audio
        audio, _, event = module.speak("dialog-warning", command="SOUND_ICON")
        assert event == "702 END" and module.icons == ["dialog-warning"]

        step("capitals")
        module.set(cap_let_recogn="spell")
        spoken, _, _ = module.speak("A", command="CHAR")
        module.set(cap_let_recogn="none")
        plain, _, _ = module.speak("A", command="CHAR")
        assert len(spoken) > len(plain)
        module.set(cap_let_recogn="icon")
        audio, _, _ = module.speak("A", command="CHAR")
        assert module.icons == ["capital"] and audio
        module.set(cap_let_recogn="none")

        step("punctuation none drops what the server already named")
        dropped, _, _ = module.speak("hello # world")
        module.set(punctuation_mode="all")
        kept, _, _ = module.speak("hello # world")
        module.set(punctuation_mode="none")
        assert len(dropped) < len(kept)

        step("a tilde in the text is not a command")
        audio, _, event = module.speak("one ~| two.")
        assert event == "702 END"
        assert audio == module.speak("one    two.")[0]

        step("stop")
        long_text = " ".join(["This sentence keeps going so that there is something to stop."] * 20)
        audio, _, event = module.speak(long_text, interrupt="STOP")
        assert event == "703 STOP"
        full = len(direct(bstspeak, "1995", long_text, "ascii"))
        assert len(audio) < full // 4, (len(audio), full)

        step("pause at the next index mark")
        pause_text = ('<speak>First part of the text, long enough to pause in.'
                      '<mark name="__spd_1"/> Second part.</speak>')
        audio, marks, event = module.speak(pause_text, interrupt="PAUSE")
        assert event == "704 PAUSE" and audio and marks == ["__spd_1"]

        step("quit")
        assert module.close() == 0
        print("speechd: all checks passed")
    finally:
        if module.process.poll() is None:
            module.process.kill()


if __name__ == "__main__":
    main()
