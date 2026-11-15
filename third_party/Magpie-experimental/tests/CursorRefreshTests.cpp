#include "../src/Magpie.Core/CursorRefreshPolicy.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace Magpie;
using namespace std::chrono_literals;
using Clock = CursorRefreshPolicy::Clock;
static int checks = 0;
static void Check(bool result, const char* message) {
	++checks;
	if (!result) { std::cerr << message << '\n'; std::exit(1); }
}
static const Clock::time_point epoch{};
static CursorVisualState Cursor(int x, uintptr_t handle = 1) {
	return {handle, x, 20, true, false, false};
}
static void Publish(CursorRefreshPolicy& policy, CursorVisualState state,
	Clock::time_point now, bool success = true) {
	policy.Prepare(state, now);
	policy.Presented(success, now);
}

int main() {
	for (bool original : {false, true}) for (bool minimum : {false, true}) {
		CursorRefreshPolicy p;
		p.Configure({original, minimum, 60}, 144);
		p.ObserveContent({1, 1, 1}, false);
		Publish(p, Cursor(0), epoch);
		Check(!p.NeedsRedraw(Cursor(0), epoch + 1s), "Unchanged cursor requested a replay");
		Check(p.NeedsRedraw(Cursor(1), epoch + 1ms) == !original, "Early independent refresh decision");
		Check(p.NeedsRedraw(Cursor(1), epoch + 17ms) == (!original || minimum), "Minimum refresh decision");
		p.ObserveContent({2, 1, 1}, true);
		Check(p.Prepare(Cursor(2), epoch + 8ms).x == (original ? 0 : 2), "Generated frame bypassed policy");
		p.Presented(true, epoch + 8ms);
		p.ObserveContent({2, 1, 1}, false);
		Check(p.Prepare(Cursor(3), epoch + 9ms).x == 3, "Original with generated ID was not admitted");
		p.Presented(true, epoch + 9ms);
		p.ObserveContent({2, 1, 1}, false);
		Check(p.Prepare(Cursor(4), epoch + 10ms).x == (original ? 3 : 4), "Replay/parameter redraw treated as new original");
		p.Presented(true, epoch + 10ms);
	}

	CursorRefreshPolicy p;
	p.Configure({true, true, 60}, 144);
	p.ObserveContent({1, 1, 1}, false);
	Publish(p, Cursor(0), epoch);
	for (int ms : {4, 8, 12, 16}) {
		p.ObserveContent({2, 1, 1}, true);
		Check(p.Prepare(Cursor(ms), epoch + std::chrono::milliseconds(ms)).x == 0, "Cached snapshot changed early");
		p.Presented(true, epoch + std::chrono::milliseconds(ms));
	}
	Check(p.MinimumDue(Cursor(17), epoch + 17ms), "Cached FG/UI redraw postponed deadline");
	Check(p.Prepare(Cursor(17), epoch + 17ms).x == 17, "Due supplement not carried by generated frame");
	p.Presented(false, epoch + 17ms);
	Check(p.MinimumDue(Cursor(18), epoch + 18ms), "Failed presentation committed cursor time");
	Publish(p, Cursor(18), epoch + 18ms);
	Check(!p.MinimumDue(Cursor(19), epoch + 19ms), "Successful supplement did not re-anchor clock");
	Check(p.PollInterval(epoch + 34ms) < 1ms, "Polling missed imminent cursor deadline");
	Check(p.Prepare(Cursor(999), epoch + 10s).x == 999, "Stall did not keep latest input");
	p.Presented(true, epoch + 10s);
	Check(!p.NeedsRedraw(Cursor(1000), epoch + 10s + 1ms), "Stall caused catch-up burst");

	// Real content at 120 FPS remains eligible with a 60 FPS minimum.
	for (int i = 0; i < 120; ++i) {
		p.ObserveContent({uint64_t(i + 10), 1, 1}, false);
		Check(p.Prepare(Cursor(i), epoch + 11s + std::chrono::microseconds(i * 8333)).x == i,
			"Minimum target capped real content cadence");
		p.Presented(true, epoch + 11s + std::chrono::microseconds(i * 8333));
	}

	// Strict mode permits ownership/visibility handoffs, but not toolbars
	// repeatedly publishing fresh coordinates on their ordinary refreshes.
	p.Configure({true, false, 60}, 60);
	p.ObserveContent({1, 1, 1}, false);
	Publish(p, Cursor(0), epoch);
	Check(!p.NeedsRedraw(Cursor(1), epoch + 1h), "Strict mode supplemented static content");
	auto transition = Cursor(2); transition.captured = false;
	Check(p.NeedsRedraw(transition, epoch + 1ms), "Ownership handoff held a stale cursor");
	Publish(p, transition, epoch + 1ms);
	transition.x = 3;
	Check(p.Prepare(transition, epoch + 2ms).x == 2, "Ordinary toolbar refresh bypassed strict mode");
	p.Presented(true, epoch + 2ms);
	transition.toolbarMove = true;
	Publish(p, transition, epoch + 3ms);
	transition.handle = 0; transition.suppressed = true;
	Check(p.NeedsRedraw(transition, epoch + 4ms), "Screenshot/visibility hide not immediate");
	Publish(p, transition, epoch + 4ms);
	transition.x = 999;
	Check(!p.NeedsRedraw(transition, epoch + 1h), "Hidden cursor burned presentations");
	transition.suppressed = false; transition.handle = 1;
	Check(p.NeedsRedraw(transition, epoch + 5ms), "Visibility restoration held stale cursor");

	// XeSS acknowledges unchanged raw submissions without empty UI commits;
	// failed UI commits remain eligible for retry even on the same input.
	p.Configure({true, false, 60}, 144);
	p.ObserveContent({1, 1, 1}, false);
	Publish(p, Cursor(0), epoch);
	p.ObserveContent({2, 1, 1}, false);
	p.AcknowledgeUnchangedOriginal(Cursor(0));
	Check(!p.HasNewOriginal(), "Unchanged XeSS submission left an old cursor opportunity");
	Check(!p.NeedsRedraw(Cursor(1), epoch + 1ms), "Old XeSS original admitted a later mouse move");
	p.ObserveContent({3, 1, 1}, false);
	Publish(p, Cursor(2), epoch + 2ms, false);
	Check(p.NeedsRedraw(Cursor(3), epoch + 3ms), "Failed independent UI lost original update");
	Publish(p, Cursor(3), epoch + 3ms);
	Check(!p.HasNewOriginal(), "Successful UI did not consume original");
	for (auto identity : {CursorContentIdentity{3, 2, 1}, CursorContentIdentity{3, 2, 2}}) {
		p.ObserveContent(identity, false);
		Check(p.HasNewOriginal(), "Sequence/resource epoch lost");
		Publish(p, Cursor(4), epoch + 4ms);
	}
	p.ResetVisual();
	Check(p.NeedsRedraw(Cursor(5), epoch + 5ms), "Resize did not invalidate old coordinates");

	p.Configure({true, true, 60}, 30);
	Publish(p, Cursor(0), epoch);
	Check(!p.MinimumDue(Cursor(1), epoch + 17ms), "Target ignored display capacity");
	Check(p.MinimumDue(Cursor(1), epoch + 34ms), "Display-limited cursor deadline lost");
	CursorRefreshPolicy separate;
	separate.Configure({true, false, 60}, 144);
	Check(separate.NeedsRedraw(Cursor(10), epoch), "Session leaked old cursor state");

	for (double value : {0.0, -1.0, 1001.0, std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::quiet_NaN()}) {
		Check(CursorRefreshSettings::ValidateRate(value) == 60, "Invalid FPS did not default to 60");
	}
	Check(CursorRefreshSettings::ValidateRate(1) == 1 && CursorRefreshSettings::ValidateRate(1000) == 1000,
		"Valid FPS boundaries rejected");
	p.Configure({true, true, 60}, 144);
	Check(!p.Presented(true, epoch), "No prepared image counted as publication");
	p.Prepare(Cursor(1), epoch);
	Check(!p.Presented(false, epoch), "Failed image counted as publication");
	p.Prepare(Cursor(1), epoch);
	Check(p.Presented(true, epoch), "Changed successful image not counted");
	p.Prepare(Cursor(1), epoch + 1s);
	Check(!p.Presented(true, epoch + 1s), "Cached image counted as publication");
	std::cout << "Cursor publication policy: " << checks << " checks passed\n";
}
