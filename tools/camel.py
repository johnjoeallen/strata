#!/usr/bin/env python3
"""camel.py - rename the snake_case Strata identifiers listed in NAMES to camelCase in the files given. usage: camel.py file..."""
import re, sys
NAMES = """append_i64 append_json bad_request cache_get cache_put db_query encode_b64 group_by handle_get_user handle_report headers_too_large
is_dir is_some keep_alive load_rows log_event log_ring max_batch max_hold not_found not_implemented ok_close ok_ka parse_id parse_range
payload_too_large read_dir read_row render_report render_user row_to_user send_query server_error sort_by split_last to_i64 try_push
unsafe_busy verify_token write_all write_json write_nowait get_user""".split()
def camel(s): a = s.split("_"); return a[0] + "".join(p[:1].upper() + p[1:] for p in a[1:])
pat = re.compile(r"\b(" + "|".join(NAMES) + r")\b")
for f in sys.argv[1:]:
    t = open(f).read(); n = pat.sub(lambda m: camel(m.group(1)), t)
    if n != t: open(f, "w").write(n); print("changed", f)
