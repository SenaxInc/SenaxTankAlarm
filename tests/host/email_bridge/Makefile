# Host test for the Google Apps Script email bridge on the server's /email-setup page
# (the `var CODE=[...]` array in EMAIL_SETUP_HTML).
#
#   make test
#
# Needs node (18 or later). ARDUINOJSON_DIR, which CI passes to every suite, is not used.

NODE ?= node

.PHONY: test

test:
	$(NODE) email_bridge_test.js
