"""Tests for minify.py: the minifier rules, and index_html.h matching web/index.html.

    python -m unittest tests/test_minify.py -v
"""
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import minify  # noqa: E402


class MinifyJs(unittest.TestCase):
    def check(self, source, expected):
        self.assertEqual(minify.minify_js(source), expected)

    def test_removes_comments_and_spaces(self):
        self.check("let a = 1; // one\n/* two\nlines */ let b = 2;", "let a=1;let b=2;")

    def test_keeps_one_space_between_words(self):
        self.check("const   x = async e => { return typeof e; };", "const x=async e=>{return typeof e;};")

    def test_keeps_strings_as_they_are(self):
        self.check("const u = 'http://x//y'; // note", "const u='http://x//y';")
        self.check('const s = "/* not */ a  b";', 'const s="/* not */ a  b";')
        self.check(r"const s = 'it\'s // ok';", r"const s='it\'s // ok';")
        self.check("const t = `a  ${b}  c`;", "const t=`a  ${b}  c`;")

    def test_keeps_regular_expressions(self):
        self.check(r"const r = /a\/b[/]c/g.test(x); // c", r"const r=/a\/b[/]c/g.test(x);")
        self.check("function f(){ return /x y/.source; }", "function f(){return/x y/.source;}")

    def test_division_is_not_a_regex(self):
        self.check("const h = a / 2 / b;", "const h=a/2/b;")

    def test_keeps_space_between_plus_or_minus_signs(self):
        self.check("const d = a - -b + +c;", "const d=a- -b+ +c;")

    def test_errors(self):
        for source in ("const s = 'open", "/* open", "x = /open"):
            with self.subTest(source=source), self.assertRaises(ValueError):
                minify.minify_js(source)


class MinifyHtml(unittest.TestCase):
    def test_line_breaks_next_to_tags_are_removed(self):
        html = "<p>\n  Hello\n  <b>world</b> and\n  more\n</p>\n<!-- gone -->\n<i>a</i> <i>b</i>"
        self.assertEqual(minify.minify_html(html), "<p>Hello<b>world</b> and more</p><i>a</i> <i>b</i>")

    def test_scripts_are_minified_as_javascript(self):
        html = "<div>\n  <script>\n    const a = 1; // x\n  </script>\n</div>"
        self.assertEqual(minify.minify(html), "<div><script>const a=1;</script></div>")

    def test_header_refuses_the_raw_string_delimiter(self):
        with self.assertRaises(ValueError):
            minify.header('<p>)rawliteral"</p>')


class RealPage(unittest.TestCase):
    def setUp(self):
        self.page, self.content = minify.build()

    def test_header_is_up_to_date(self):
        self.assertTrue(minify.header_up_to_date(), "index_html.h is out of date: run `python minify.py`")

    def test_no_comments_or_line_breaks_left(self):
        self.assertNotIn("<!--", self.page)
        self.assertNotIn("\n", self.page)
        self.assertNotIn("//", re.sub(r"https?://", "", self.page))

    def test_page_is_complete(self):
        self.assertTrue(self.page.startswith("<!DOCTYPE html>"))
        self.assertTrue(self.page.endswith("</html>"))
        self.assertEqual(len(re.findall(r"<script\b", self.page)), 3)

    def test_page_fits_comfortably_in_flash(self):
        self.assertLess(len(self.page.encode()), 16 * 1024)


if __name__ == "__main__":
    unittest.main()
