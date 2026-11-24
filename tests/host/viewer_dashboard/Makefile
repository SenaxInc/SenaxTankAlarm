# Host test for the viewer's dashboard page (VIEWER_DASHBOARD_HTML in the Viewer sketch).
#
#   make test
#
# Needs node (18 or later). ARDUINOJSON_DIR, which CI passes to every suite, is not used.

NODE ?= node

.PHONY: test

test:
	$(NODE) viewer_dashboard_test.js
