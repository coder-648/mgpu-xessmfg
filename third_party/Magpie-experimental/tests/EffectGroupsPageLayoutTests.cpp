// Native WinUI resources and production SimpleStackPanel; no Magpie/config startup.
#include "pch.h"
#include "SimpleStackPanel.h"
#include "EffectGroupsPageFixture.h"
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using namespace winrt;

struct LayoutTestApp : ApplicationT<LayoutTestApp, Markup::IXamlMetadataProvider> {
	winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider{ nullptr };
	auto& Provider() {
		if (!provider) provider = winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider();
		return provider;
	}
	Markup::IXamlType GetXamlType(Interop::TypeName const& type) { return Provider().GetXamlType(type); }
	Markup::IXamlType GetXamlType(hstring const& type) { return Provider().GetXamlType(type); }
	com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return Provider().GetXmlnsDefinitions(); }
};

auto MakePanel(Orientation orientation = Orientation::Vertical) {
	auto panel = make_self<Magpie::implementation::SimpleStackPanel>();
	panel->Orientation(orientation);
	return panel;
}

int main() {
	try {
		init_apartment(apartment_type::single_threaded);
		const auto app = make_self<LayoutTestApp>();
		const auto manager = Hosting::WindowsXamlManager::InitializeForCurrentThread();
		app->Provider();
		MUXC::XamlControlsResources winui;
		winui.ControlsResourcesVersion(MUXC::ControlsResourcesVersion::Version2);
		app->Resources().MergedDictionaries().Append(winui);
		app->Resources().Insert(box_value(L"StandardIconSize"), box_value(14.0));
		const auto styles = Markup::XamlReader::Load(HeaderStyleXaml).as<ResourceDictionary>();
		app->Resources().MergedDictionaries().Append(styles);
		const auto headerStyle = styles.Lookup(box_value(L"ScalingModesHeaderButtonStyle")).as<Style>();

		// The old negative margin produces an empty desired extent and is skipped
		// by the production panel. Use the actual AccentButtonStyle/New button.
		const auto parent = MakePanel();
		const auto oldFooter = MakePanel();
		oldFooter->Margin({ 0, -36, 0, 0 });
		const auto oldButton = Markup::XamlReader::Load(NewButtonXaml).as<Button>();
		oldFooter->Children().Append(oldButton);
		parent->Children().Append(oldFooter.as<UIElement>());
		parent->Measure({ 400, 400 });
		assert(oldFooter->DesiredSize().Height == 0);
		parent->Arrange({ 0, 0, 400, 400 });
		assert(oldButton.ActualHeight() == 0);
		std::cout << "BASELINE negative margin: footer desired=0, button actual=0.\n";

		size_t headerCases = 0;
		float widestActions = 0;
		for (const auto& language : LanguageCases) {
			for (const auto theme : { ElementTheme::Light, ElementTheme::Dark }) {
				for (const float width : { 452.f, 592.f, 852.f, 1252.f }) {
					const auto header = Markup::XamlReader::Load(HeaderGridXaml).as<Grid>();
					header.RequestedTheme(theme);
					const float edge = width > 590 ? 40.f : 25.f;
					header.Margin({ edge, 54, edge, 16 });
					const auto actions = MakePanel(Orientation::Horizontal);
					actions->Spacing(HeaderSpacing);
					actions->Resources(Markup::XamlReader::Load(HeaderActionResourcesXaml).as<ResourceDictionary>());
					std::vector<Button> buttons;
					for (const auto* label : language.labels) {
						Button button;
						button.Style(headerStyle);
						// Materialize the text so the unattached fixture measures
						// glyphs rather than relying on deferred string content.
						TextBlock text;
						text.Text(label);
						text.FontSize(button.FontSize());
						button.Content(text);
						actions->Children().Append(button);
						buttons.push_back(button);
					}
					const auto presenter = header.FindName(L"HeaderActionPresenter").as<ContentControl>();
					const auto menu = Markup::XamlReader::Load(OptionsMenuXaml).as<MenuFlyout>();
					assert(menu.Items().Size() == 5 && menu.Items().GetAt(3).try_as<MenuFlyoutSeparator>());
					assert(menu.Placement() == Primitives::FlyoutPlacementMode::Bottom);
					assert(!menu.ShouldConstrainToRootBounds());
					unsigned menuLabel = 0;
					for (const auto index : {0u, 1u, 2u, 4u}) {
						const auto item = menu.Items().GetAt(index).as<MenuFlyoutItem>();
						item.Text(language.menuLabels[menuLabel++]);
						assert(item.Icon().try_as<FontIcon>() && !item.Text().empty());
					}
					buttons.front().Flyout(menu);
					presenter.Content(actions.as<IInspectable>());
					for (const auto& child : header.Children()) {
						if (const auto title = child.try_as<TextBlock>()) title.Text(language.title);
					}
					header.Measure({ width, 150 });
					header.Arrange({ 0, 0, width, 150 });
					const auto extent = actions->DesiredSize().Width;
					if (extent > width - 2 * edge) {
						std::cerr << to_string(language.language) << " header too wide: " << extent << "\n";
						return 1;
					}
					widestActions = std::max(widestActions, extent);
					float right = 0;
					for (const auto& button : buttons) {
						// Exercise the page's actual resource scope under PageFrame's
						// transparent header overrides, in both supported themes.
						assert(button.Background().as<Media::SolidColorBrush>().Color().A > 0);
						const auto border = button.BorderThickness();
						assert(border.Left == 0 && border.Top == 0 && border.Right == 0 && border.Bottom == 0);
						const auto text = button.Content().as<TextBlock>();
						assert(text.ActualWidth() > 0);
						assert(std::isnan(button.Width()));
						assert(button.IsTabStop() && button.UseSystemFocusVisuals());
						assert(button.ActualHeight() >= 32);
						assert(std::abs(button.ActualWidth() - button.DesiredSize().Width) < .1f);
						const auto point = button.TransformToVisual(actions.as<UIElement>()).TransformPoint({ 0, 0 });
						assert(point.X >= right - .1f);
						assert(point.X + button.ActualWidth() <= extent + .1f);
						assert(std::abs(point.Y - buttons.front().TransformToVisual(actions.as<UIElement>()).TransformPoint({0,0}).Y) < .1f);
						right = point.X + static_cast<float>(button.ActualWidth());
					}
					// Apply PageFrame's original icon style explicitly in this
					// runtime-created fixture; the named text style stays local.
					Button icon;
					for (const auto& resource : presenter.Resources()) {
						if (const auto style = resource.Value().try_as<Style>();
							style && style.TargetType().Name == xaml_typename<Button>().Name) icon.Style(style);
					}
					assert(icon.Style());
					presenter.Content(icon);
					header.InvalidateMeasure(); header.Measure({ width, 150 });
					assert(std::isfinite(icon.Width()) && icon.Width() > 0);
					assert(buttons.front().Height() == icon.Height());
					if (headerCases == 0) {
						std::cout << "Legacy entrance height=" << icon.Height()
							<< " DIP; options width=" << extent << " DIP.\n";
					}
					++headerCases;
				}
			}
		}
		size_t footerCases = 0;
		for (const auto theme : { ElementTheme::Light, ElementTheme::Dark }) {
			for (const float width : { 402.f, 512.f, 772.f, 1000.f }) {
				const auto content = MakePanel();
				content->RequestedTheme(theme);
				ListView list;
				list.Padding(ListPadding);
				list.SelectionMode(ListViewSelectionMode::None);
				content->Children().Append(list);
				const auto footer = MakePanel();
				footer->Margin(FooterMargin);
				const auto button = Markup::XamlReader::Load(NewButtonXaml).as<Button>();
				footer->Children().Append(button);
				content->Children().Append(footer.as<UIElement>());
				// Dynamic resize/add/remove/expanded-size changes, including empty.
				for (const unsigned count : { 0u, 1u, 20u, 1u, 0u }) {
					list.Items().Clear();
					for (unsigned i = 0; i < count; ++i) {
						Border item;
						item.Height(i % 2 == 0 ? 60 : 180);
						list.Items().Append(item);
					}
					content->InvalidateMeasure();
					content->Measure({ width, std::numeric_limits<float>::infinity() });
					content->Arrange({ 0, 0, width, content->DesiredSize().Height });
					assert(footer->DesiredSize().Height > 0 && button.ActualHeight() > 0);
					const auto point = button.TransformToVisual(content.as<UIElement>()).TransformPoint({0,0});
					assert(point.Y >= list.DesiredSize().Height - .1f);
					assert(std::abs(point.X + button.ActualWidth() - width) < .1f);
					assert(point.Y + button.ActualHeight() <= content->DesiredSize().Height + .1f);
					++footerCases;
				}
			}
		}
		std::cout << "PASS native layout: " << headerCases << " localized/theme/width headers, "
			<< footerCases << " dynamic footers; widest actions=" << widestActions << " DIP.\n";
		manager.Close();
		return 0;
	} catch (hresult_error const& error) {
		std::cerr << "XAML failure 0x" << std::hex << static_cast<uint32_t>(error.code().value)
			<< ": " << to_string(error.message()) << "\n";
		return 1;
	}
}
