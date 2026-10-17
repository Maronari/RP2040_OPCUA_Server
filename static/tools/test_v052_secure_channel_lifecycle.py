#!/usr/bin/env python3
"""Guard the v0.5.2 open62541 SecureChannel lifecycle pairing patch."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "port/open62541/open62541.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require(
    "UA_Boolean applicationLifecycleNotified;" in SOURCE,
    "SecureChannel lacks the application lifecycle pairing state",
)

delete_start = SOURCE.index("deleteServerSecureChannel(")
delete_end = SOURCE.index("\n}\n\nUA_StatusCode\nsendServiceFault", delete_start)
delete_body = SOURCE[delete_start:delete_end]
require(
    "if(channel->applicationLifecycleNotified)" in delete_body
    and "channel->applicationLifecycleNotified = false;" in delete_body
    and "UA_APPLICATIONNOTIFICATIONTYPE_SECURECHANNEL_CLOSED" in delete_body,
    "SecureChannel destruction does not guard and consume the paired CLOSED notification",
)
require(
    delete_body.index("if(channel->applicationLifecycleNotified)")
    < delete_body.index("UA_APPLICATIONNOTIFICATIONTYPE_SECURECHANNEL_CLOSED"),
    "SecureChannel CLOSED notification is reachable before the lifecycle guard",
)

open_marker = "if(request->requestType == UA_SECURITYTOKENREQUESTTYPE_ISSUE)"
open_start = SOURCE.index(open_marker, SOURCE.index("Service_OpenSecureChannel"))
open_end = SOURCE.index("\n    } else {", open_start)
open_body = SOURCE[open_start:open_end]
require(
    "channel->applicationLifecycleNotified = true;" in open_body
    and "UA_APPLICATIONNOTIFICATIONTYPE_SECURECHANNEL_OPENED" in open_body,
    "successful initial OpenSecureChannel does not arm the paired lifecycle",
)
require(
    open_body.index("channel->applicationLifecycleNotified = true;")
    < open_body.index("UA_APPLICATIONNOTIFICATIONTYPE_SECURECHANNEL_OPENED"),
    "SecureChannel lifecycle state must be armed before OPENED notification",
)

require(
    SOURCE.count("channel->applicationLifecycleNotified = true;") == 1,
    "SecureChannel lifecycle must be armed only for the first successful ISSUE request",
)

print("v0.5.2 SecureChannel lifecycle source test: PASS")
