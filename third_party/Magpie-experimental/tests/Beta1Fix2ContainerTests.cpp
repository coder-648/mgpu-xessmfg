// Actual production panel, native ItemsControl-generated containers; no visible window.
#include "pch.h"
#include "SimpleStackPanel.h"
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <cassert>
#include <iostream>
#include <limits>

int main() {
	using namespace winrt;
	init_apartment(apartment_type::single_threaded);
	const auto manager = Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
	const auto production = make_self<Magpie::implementation::SimpleStackPanel>();
	const auto panel = production.as<Windows::UI::Xaml::Controls::Panel>();
	ItemsControl items;
	items.ItemTemplate(Windows::UI::Xaml::Markup::XamlReader::Load(
		LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><Grid Width="260" Height="36" /></DataTemplate>)").as<DataTemplate>());
	const auto values = single_threaded_vector<IInspectable>();
	for (int i=0;i<5;++i) values.Append(box_value(i));
	items.ItemsSource(values);
	items.Measure({3000,3000}); items.Arrange({0,0,3000,3000});
	const auto nativeRoot = items.ItemsPanelRoot().as<Panel>();
	assert(nativeRoot.Children().Size()==5);
	std::vector<UIElement> containers;
	std::vector<Grid> grids;
	for (const auto& child : nativeRoot.Children()) {
		assert(child.try_as<ContentPresenter>());
		containers.push_back(child);
		grids.push_back(Windows::UI::Xaml::Media::VisualTreeHelper::GetChild(child,0).as<Grid>());
	}
	nativeRoot.Children().Clear();
	for (const auto& child : containers) panel.Children().Append(child);
	for (const auto orientation : {Orientation::Horizontal,Orientation::Vertical}) {
		production->Orientation(orientation);
		production->Spacing(orientation==Orientation::Horizontal ? 24 : 8);
		for (unsigned mask=0; mask<32; ++mask) {
			unsigned visible=0;
			for (unsigned i=0;i<5;++i) {
				const bool show=(mask&(1u<<i))!=0; visible+=show;
				grids[i].Visibility(show ? Visibility::Visible : Visibility::Collapsed);
				assert(containers[i].Visibility()==Visibility::Visible);
			}
			panel.InvalidateMeasure(); panel.Measure({3000,3000});
			const auto size=panel.DesiredSize();
			const float extent=orientation==Orientation::Horizontal ? size.Width : size.Height;
			const float cell=orientation==Orientation::Horizontal ? 260.f : 36.f;
			const float gap=orientation==Orientation::Horizontal ? 24.f : 8.f;
			const float expected=visible ? visible*cell+(visible-1)*gap : 0;
			assert(std::abs(extent-expected)<.01f);
			panel.Arrange({0,0,size.Width,size.Height});
			float offset=0;
			for (unsigned i=0;i<5;++i) if (mask&(1u<<i)) {
				const auto position=containers[i].as<FrameworkElement>().TransformToVisual(panel).TransformPoint({0,0});
				assert(std::abs((orientation==Orientation::Horizontal ? position.X : position.Y)-offset)<.01f);
				offset+=cell+gap;
			}
		}
	}
	std::cout << "PASS production SimpleStackPanel: real ItemsControl presenters, 64 horizontal/vertical visibility combinations, dynamic measuring and arrangement without phantom gaps.\n";
}
