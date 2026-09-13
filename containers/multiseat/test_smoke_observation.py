import copy
import unittest

from smoke_observation import (
    ExpectedSeat, ObservationError, is_provider_diagnostic,
    observe_workers, seat_transitions,
)


SEATS = (ExpectedSeat("steam", "pv-steam", "sha256:" + "a" * 64),
         ExpectedSeat("comparison", "pv-comparison", "sha256:" + "b" * 64))


def worker(seat, identity):
    return {
        "Id": identity * 64, "Image": seat.image,
        "Config": {"Labels": {
            "io.polaris.multiseat.volume": seat.volume,
            "io.polaris.multiseat.deployment": "test-deployment",
        }},
        "State": {"Running": True, "Paused": False, "Restarting": False,
                  "Status": "running", "StartedAt": "start-one"},
        "Mounts": [{"Destination": "/var/lib/polaris-seat", "Type": "volume",
                    "Name": seat.volume, "RW": True}],
    }


def observe(records, seats=SEATS):
    return observe_workers(records, "test-deployment", seats)


class SmokeObservationTests(unittest.TestCase):
    def setUp(self):
        self.steam = worker(SEATS[0], "1")
        self.comparison = worker(SEATS[1], "2")
        self.both = observe([self.steam, self.comparison])

    def test_comparison_disappearance_does_not_retire_steam(self):
        after = observe([self.steam])
        self.assertTrue(after["steam"]["running"])
        self.assertFalse(after["comparison"]["running"])
        changes = seat_transitions(self.both, after)
        self.assertEqual([(c["seat"], c["event"]) for c in changes],
                         [("comparison", "disappeared")])

    def test_steam_disappearance_is_attributed_to_steam(self):
        changes = seat_transitions(self.both, observe([self.comparison]))
        self.assertEqual([(c["seat"], c["event"]) for c in changes],
                         [("steam", "disappeared")])

    def test_same_count_does_not_hide_a_replacement(self):
        replacement = worker(SEATS[0], "3")
        changes = seat_transitions(self.both, observe([replacement, self.comparison]))
        self.assertEqual([(c["seat"], c["event"]) for c in changes], [("steam", "replaced")])

    def test_same_id_restart_is_a_new_lifetime(self):
        restarted = copy.deepcopy(self.steam)
        restarted["State"]["StartedAt"] = "start-two"
        changes = seat_transitions(self.both, observe([restarted, self.comparison]))
        self.assertEqual([(c["seat"], c["event"]) for c in changes], [("steam", "restarted")])

    def test_inventory_order_does_not_create_transitions(self):
        self.assertEqual(seat_transitions(self.both, observe([self.comparison, self.steam])), [])

    def test_missing_and_stopped_workers_are_distinct(self):
        stopped = copy.deepcopy(self.comparison)
        stopped["State"].update(Running=False, Status="exited")
        states = observe([stopped])
        self.assertEqual(states["steam"]["state"], "absent")
        self.assertEqual(states["comparison"]["state"], "exited")
        self.assertFalse(any(s["running"] for s in states.values()))

    def test_paused_and_restarting_workers_are_not_running_evidence(self):
        for flag in ("Paused", "Restarting"):
            with self.subTest(flag=flag):
                record = copy.deepcopy(self.steam)
                record["State"][flag] = True
                self.assertFalse(observe([record])["steam"]["running"])

    def test_container_health_does_not_claim_playback(self):
        for state in self.both.values():
            self.assertNotIn("playback_verified", state)
            self.assertNotIn("streaming", state)

    def test_initial_observation_only_reports_present_workers(self):
        changes = seat_transitions(None, observe([self.comparison]))
        self.assertEqual([(c["seat"], c["event"]) for c in changes], [("comparison", "appeared")])

    def test_empty_inventory_is_distinct_from_an_unavailable_inventory(self):
        self.assertFalse(any(s["running"] for s in observe([]).values()))
        for unavailable in (None, {}, "unavailable"):
            with self.subTest(unavailable=unavailable), self.assertRaises(ObservationError):
                observe(unavailable)

    def test_duplicate_workers_cannot_masquerade_as_two_seats(self):
        for duplicate in (self.steam, worker(SEATS[0], "3")):
            with self.subTest(duplicate=duplicate["Id"]), self.assertRaises(ObservationError):
                observe([self.steam, duplicate])

    def test_inventory_drift_is_rejected(self):
        mutations = [
            lambda r: r.update(Id="short"),
            lambda r: r.update(Image=SEATS[1].image),
            lambda r: r["Config"]["Labels"].update({"io.polaris.multiseat.volume": "pv-unknown"}),
            lambda r: r["Config"]["Labels"].update({"io.polaris.multiseat.deployment": "another-test"}),
            lambda r: r["Mounts"][0].update(Name=SEATS[1].volume),
            lambda r: r["Mounts"][0].update(Type="bind"),
            lambda r: r["Mounts"][0].update(RW=False),
            lambda r: r["Mounts"].append(copy.deepcopy(r["Mounts"][0])),
            lambda r: r.update(Mounts=[]),
            lambda r: r["State"].update(Running="true"),
            lambda r: r["State"].update(Status="unknown"),
            lambda r: r["State"].update(StartedAt=None),
            lambda r: r.pop("State"),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(mutation=index), self.assertRaises(ObservationError):
                record = copy.deepcopy(self.steam)
                mutate(record)
                observe([record])

    def test_same_image_can_serve_different_profiles_without_confusing_identity(self):
        other = ExpectedSeat("comparison", "pv-comparison", SEATS[0].image)
        state = observe([self.steam, worker(other, "2")], (SEATS[0], other))
        self.assertEqual(state["steam"]["worker_id"], "1" * 64)
        self.assertEqual(state["comparison"]["worker_id"], "2" * 64)

    def test_ambiguous_expected_seats_are_rejected(self):
        for seats in ([], iter(()), (SEATS[0], SEATS[0]),
                      (SEATS[0], ExpectedSeat("comparison", SEATS[0].volume, SEATS[1].image)),
                      (ExpectedSeat("steam", "pv-steam", "latest"),)):
            with self.subTest(seats=seats), self.assertRaises(ObservationError):
                observe([], seats)

    def test_both_provider_log_formats_are_retained(self):
        for line in ("runtime provider failed: runtime display exited unexpectedly\n",
                     "polaris-seat-display-capture: runtime display exited unexpectedly (signal 11)\n",
                     "polaris-seat-display-capture: capture produced no frame within the one-second activity deadline\n",
                     "polaris-seat-encoder: encoder pipeline: format negotiation failed\n",
                     "polaris-seat-worker: worker runtime display-capture exited unexpectedly\n"):
            with self.subTest(line=line):
                self.assertTrue(is_provider_diagnostic(line))
        for line in ("Steam account log", "prefix runtime provider failed: something",
                     "polaris-seat-unknown: not a provider", None):
            with self.subTest(line=line):
                self.assertFalse(is_provider_diagnostic(line))


if __name__ == "__main__":
    unittest.main()
