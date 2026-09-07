"""
Portable replacement for the `cat qstrdefs... | sed ... | cpp ... | sed ...`
shell pipeline used to build qstrdefs.preprocessed.h. That pipeline needs a
POSIX shell (cat/sed) and can exceed cmd.exe's command-line length limit via
the cflags list, neither of which is available/safe on native Windows.

Steps: concatenate the input files, wrap each "Q(...)" line in double quotes
(so the C preprocessor treats its content as an opaque string literal rather
than macro-expanding it), run the result through the C preprocessor to expand
any config macros used elsewhere in the file, then strip the quotes back off
the Q(...) lines.
"""
import re
import subprocess
import sys

_Q_LINE = re.compile(rb"^(Q\(.*\))\r?$", re.MULTILINE)
_Q_LINE_QUOTED = re.compile(rb'^"(Q\(.*\))"\r?$', re.MULTILINE)


def main():
    *input_files, cflags_rsp, compiler, output_file = sys.argv[1:]

    data = b"".join(open(f, "rb").read() for f in input_files)
    # Some input files (e.g. the auto-collected qstrdefs) have CRLF line
    # endings; capture just the Q(...) content (group 1) so a trailing \r
    # stays outside the quotes instead of getting embedded right before the
    # closing ", which the preprocessor then treats as an unterminated
    # string (a lone \r is itself a line-ending character to it).
    data = _Q_LINE.sub(lambda m: b'"' + m.group(1) + b'"', data)

    result = subprocess.run(
        [compiler, "-E", "@" + cflags_rsp, "-"],
        input=data,
        stdout=subprocess.PIPE,
        check=True,
    )
    out = _Q_LINE_QUOTED.sub(rb"\1", result.stdout)

    with open(output_file, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
