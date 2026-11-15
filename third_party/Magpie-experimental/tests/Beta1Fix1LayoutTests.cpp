#include <cassert>
#include <cmath>
#include <iostream>
#include "EffectParameterPopupLayout.h"

int main() {
	using namespace Magpie;
	for (double root : {1.0, 1.25, 1.5, 2.0}) for (double monitor : {1.0, 1.25, 1.5, 2.0}) {
		for (auto work : {EffectParameterPopupSize{1920,1040}, {1280,720}, {800,560}}) {
			const auto size = GetEffectParameterPopupSize(work.width, work.height, monitor, root);
			const double scale = std::max(root, monitor);
			assert((size.width+72)*scale <= work.width+.001);
			assert((size.height+72)*scale <= work.height+.001);
			assert(size.width>0 && size.height>0);
			for (size_t columns=1; columns<=5; ++columns) {
				const auto layout = GetEffectParameterColumnLayout(columns, size.width);
				assert(layout.columnWidth>=240 && layout.columnWidth<=260);
				assert(layout.viewportWidth<=size.width);
				assert(layout.viewportWidth<=layout.columnWidth*columns+24*(columns-1));
			}
		}
	}
	// Popup width is independent of the smaller owning window. A narrow
	// monitor uses horizontal scrolling instead of unreadably narrow columns.
	const auto wide = GetEffectParameterPopupSize(1920,1040,1,1);
	assert(GetEffectParameterColumnLayout(5,wide.width).viewportWidth==1396);
	const auto narrow = GetEffectParameterPopupSize(800,560,1,1);
	assert(GetEffectParameterColumnLayout(5,narrow.width).columnWidth==240);
	assert(GetEffectParameterColumnLayout(1,wide.width).viewportWidth==260);
	std::cout << "PASS popup layout: 48 work-area/mixed-DPI cases, 1–5 dynamic columns, readable widths and scrolling.\n";
}
