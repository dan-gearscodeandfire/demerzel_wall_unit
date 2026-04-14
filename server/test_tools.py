"""Offline unit tests for tools.py — specifically the _ha_find_entity matcher.

No HA required: we monkey-patch the _ha_get_states cache with a fake state
list and then exercise the matcher directly. Run with:

    python3 server/test_tools.py

Guard against the class of bug that landed on 2026-04-14:

    _ha_find_entity used to return None when a candidate matched via
    entity_id tokens only (not friendly_name), because the specificity
    penalty pushed the score below zero and the final gate conflated
    "no candidate" with "candidate scored negative". This meant
    "basement sitting area nook lights" could not resolve even though
    every hint token was in the entity_id.

Keep these tests passing. If you change _ha_find_entity, make sure
entity-id-only matches still resolve.
"""

import sys
import os

# Let the file run from any cwd
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import tools  # noqa: E402


FAKE_STATES = [
    # The bug case: friendly_name has NONE of the hint tokens
    {
        "entity_id": "light.basement_sitting_area_nook_lights",
        "attributes": {"friendly_name": "Dan's Office Can Light"},
        "state": "off",
    },
    # Sibling light in the same area — disambiguation target
    {
        "entity_id": "light.basement_sitting_area_main_lights",
        "attributes": {"friendly_name": "Theater Main Overhead Lights Basement Sitting Area Main Lights"},
        "state": "off",
    },
    {
        "entity_id": "light.basement_sitting_area_screen_lights",
        "attributes": {"friendly_name": "Basement Sitting Area Screen Lights"},
        "state": "off",
    },
    # Sensor with tokens in both fn and eid — classic case
    {
        "entity_id": "sensor.den_multisensor_air_temperature",
        "attributes": {
            "friendly_name": "Den Multisensor Air temperature",
            "unit_of_measurement": "°F",
        },
        "state": "61.6",
    },
    {
        "entity_id": "sensor.shop_multisensor_air_temperature",
        "attributes": {
            "friendly_name": "Shop Multisensor Air temperature",
            "unit_of_measurement": "°F",
        },
        "state": "84.7",
    },
    # Unrelated switch — should never match a light hint
    {
        "entity_id": "switch.back_door_flood",
        "attributes": {"friendly_name": "Back Door Flood"},
        "state": "off",
    },
]


def install_fake_states():
    tools._HA_STATES_CACHE["data"] = FAKE_STATES
    tools._HA_STATES_CACHE["ts"] = 10**12  # far future so force=False never refetches


def check(label, got, expected):
    ok = got == expected
    mark = "OK  " if ok else "FAIL"
    print(f"  [{mark}] {label}")
    print(f"         got      = {got!r}")
    if not ok:
        print(f"         expected = {expected!r}")
    return ok


def run():
    install_fake_states()
    passed = 0
    failed = 0

    print("== _ha_find_entity / _ha_resolve_entity ==")

    cases = [
        # (label, domain, hint, expected_entity_id)

        # REGRESSION GUARD: entity_id-word hint must resolve
        ("entity-id words: basement sitting area nook lights",
         "light", "basement sitting area nook lights",
         "light.basement_sitting_area_nook_lights"),

        # Friendly-name-word hint (yesterday's working path) must still resolve
        ("friendly-name words: dan office can lights",
         "light", "dan office can lights",
         "light.basement_sitting_area_nook_lights"),

        # Disambiguation: different sibling, not nook
        ("sibling: basement sitting area main lights",
         "light", "basement sitting area main lights",
         "light.basement_sitting_area_main_lights"),

        # Sensor: fn+eid both match
        ("sensor: den temperature",
         "sensor", "den temperature",
         "sensor.den_multisensor_air_temperature"),

        ("sensor: shop temperature",
         "sensor", "shop temperature",
         "sensor.shop_multisensor_air_temperature"),

        # Plural tolerance: user says "lights", fn has "Lights" too (case-insensitive)
        ("plural-tolerant: basement sitting area screen light",
         "light", "basement sitting area screen light",
         "light.basement_sitting_area_screen_lights"),

        # Domain filter: light hint must NOT match a switch entity
        ("domain filter: flood (light domain)",
         "light", "back door flood",
         None),

        # But the switch domain should find it
        ("domain filter: flood (switch domain)",
         "switch", "back door flood",
         "switch.back_door_flood"),

        # Non-matching garbage returns None
        ("no match: garbage hint",
         "light", "xyzzy unicorn",
         None),
    ]

    for label, domain, hint, expected in cases:
        got = tools._ha_resolve_entity(domain, hint)
        if check(label, got, expected):
            passed += 1
        else:
            failed += 1

    print()
    print(f"== {passed} passed, {failed} failed ==")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(run())
