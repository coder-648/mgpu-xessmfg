#include "pch.h"
#include "ContentDialogHelper.h"

using namespace winrt;
using namespace Windows::UI::Xaml::Controls;

namespace Magpie {

static weak_ref<ContentDialog> activeDialog{ nullptr };

IAsyncOperation<ContentDialogResult> ContentDialogHelper::ShowAsync(ContentDialog dialog) {
	assert(activeDialog == nullptr);

	activeDialog = dialog;
	auto clearActiveDialog = wil::scope_exit([] { activeDialog = nullptr; });
	ContentDialogResult result = co_await dialog.ShowAsync();
	co_return result;
}

bool ContentDialogHelper::IsAnyDialogOpen() noexcept {
	return activeDialog != nullptr;
}

void ContentDialogHelper::CloseActiveDialog() {
	if (activeDialog == nullptr) {
		return;
	}

	if (auto dialog = activeDialog.get()) {
		dialog.Hide();
	}
}

}
