# What to submit

**COMP6697001 Operating Systems - AOL Case Study**

The assignment requires three items: **the code**, **the executable** and
**the video**. Put them in one archive and upload it to Binusmaya. If the
archive is above the size limit, upload the video to the university OneDrive
and put the link inside the archive.

---

## 1. Build the submission archive

```bash
cd "/mnt/c/Users/thoma/Documents/Project OS"
./scripts/package.sh
```

The script writes `tsim-submission-<date>.zip` in the parent directory. The
archive holds the source, the built executable, the documents, the workloads
and the tests. It holds no object file and no editor backup file.

To build the archive by hand:

```bash
make clean && make && make check      # build, then confirm that the tests pass
cd .. && zip -r tsim-submission.zip "Project OS" \
    -x "*/build/*" -x "*/.git/*" -x "*.csv"
```

### Contents of the archive

```
Project OS/
+-- README.md                  what the program is, how to run it, the results
+-- Makefile
+-- src/            (11 files) the C source
+-- include/        (11 files) the headers
+-- workloads/       (4 files) the test workloads
+-- tests/           (3 files) 24 regression tests, 14 edge cases, the stress suite
+-- scripts/         (4 files) setup, demonstration, comparison, packaging
+-- docs/
|   +-- HOWTO.md               how to run it on WSL Ubuntu, and how the code works
|   +-- INSTALL_WSL.md         how to install Ubuntu under WSL 2
|   +-- DESIGN.md              how the program works, and the reason for each choice
|   +-- VIDEO_SCRIPT.md        the plan for the video
|   +-- SUBMISSION.md          this file
+-- tsim                       the built executable (ELF x86-64, Linux)
+-- VIDEO.txt                  the OneDrive link, if the video is not in the archive
```

### A note about the executable

`tsim` is an **ELF x86-64 Linux binary**. A double-click in Windows does not
run it. It runs inside WSL, or on any Linux machine.

```bash
./tsim --help
```

A marker who needs to rebuild it can run `make`.
`./scripts/setup_wsl.sh` also installs any missing package first. The
assignment asks for the code and the executable, so include both.

---

## 2. The video

**Required length: 5 to 10 minutes.** `docs/VIDEO_SCRIPT.md` holds a plan for
8 minutes, with the commands to run and the points to state. The video must
hold both of these items:

- the program in operation, and
- an explanation of how the code works.

The marks are 30% for the installation, 40% for the program and 30% for the
explanation. Give the installation a full minute, and spend more than two
minutes on the code rather than on the output.

To record on Windows, press **Win + Alt + R** for the Xbox Game Bar recorder,
or use OBS Studio. Record the whole screen with the terminal at full size and
a large font.

If the video makes the archive too large for Binusmaya, upload it to the
university OneDrive. Set the sharing so that your lecturer can open it. Put
the link in a file named `VIDEO.txt` at the top of the archive.

---

## 3. The AI use declaration form

The assignment pack includes the BINUS **Artificial Intelligence Use
Declaration Form**. The assessment form also lists an AI use type, which is
one of Integrated, Partial, Flexible, Limited or Independent.

**The pack leaves the permitted and prohibited sections empty.** They read
`< fill in permitted use of generative AI here >`. Ask your lecturer or check
the course page for the type that applies to this assignment before you
complete the form. The type changes what the form requires.

Complete the form accurately. It has one row for each part of the assignment,
and it asks for the tool and the main prompt for each row. It also asks for an
approximate percentage at the end. A declaration that does not match the work
is academic misconduct.

The explanation carries 30% of the mark, and a marker can ask about any part
of the code. Read `docs/DESIGN.md` until you can answer the questions at the
end of `docs/VIDEO_SCRIPT.md` without a reference. These five questions matter
most:

1. Why does the kernel use `SIGSTOP` instead of a flag that the process reads?
2. Why does the main loop use `sigsuspend()` instead of an unblock followed by
   `pause()`?
3. Why is the I/O request a queued real-time signal?
4. Why does `io_complete()` send `SIGSTOP` before the wakeup?
5. How do you know that only one process is runnable at a time?

---

## 4. Final checklist

```bash
make clean && make        # no warning
make check                # 38 tests, no failure
./tsim --verify           # no exclusivity violation
```

- [ ] `make` produces no compiler warning
- [ ] `make check` reports 24 and 14 tests passing, and the stress suite clean
- [ ] `./tsim --verify` reports 0 violations
- [ ] the archive holds the source, the headers and the Makefile
- [ ] the archive holds the built `tsim` executable
- [ ] the video is 5 to 10 minutes long, and it shows the program and an
      explanation of the code
- [ ] the video is in the archive, or `VIDEO.txt` holds a OneDrive link that
      your lecturer can open. Test the link in a private browser window
- [ ] the AI declaration form is complete and accurate, and the AI use type
      matches the course requirement
- [ ] your name and student ID are on the declaration form
- [ ] you can answer each of the five questions in section 3 without a
      reference
