// A window built from C++, with lambdas.
//
//   g++ -std=c++17 example.cpp -I../../include -I. -lraven -o example
//
// `example --test` runs without a window system and without a GPU, clicks its
// own button and checks what happened: it is how the build of Raven checks
// this binding.

#include <raven.hpp>

#include <cstdio>
#include <cstring>
#include <string>

#define EXPECT(condition) do { if (!(condition)) { std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; } } while (0)

int main(int argc, char** argv) {
	const bool test = argc > 1 && std::strcmp(argv[1], "--test") == 0;
	RInitDesc desc{};
	desc.backend = test ? R_BACKEND_NULL : R_BACKEND_VULKAN;
	desc.headless = test;
	desc.app_name = "Raven from C++";
	if (!raven::init_ex(&desc)) {
		std::printf("cannot start Raven\n");
		return 1;
	}

	int clicks = 0;
	std::string greeted;

	auto window = raven::Window::create("Raven from C++", 360, 200, R_WINDOW_DEFAULT);
	auto box = raven::Panel(raven::Widget::create(R_WIDGET_VBOX));
	box.set_spacing(10.0f);
	auto title = raven::Label::create("Hello from C++");
	title.set_text_size(20.0f);
	auto name = raven::TextInput::create();
	name.set_placeholder("Your name");
	auto greet = raven::Button::create("Greet");
	greet.set_style(R_BUTTON_PRIMARY);
	// a lambda with captures, where C takes a function and a pointer
	greet.set_clicked_callback([&](Rid) {
		clicks++;
		greeted = std::string("Hello, ") + name.get_text() + "!";
		title.set_text(greeted.c_str());
	});
	box.add_child(title);
	box.add_child(name);
	box.add_child(greet);
	box.slot_set_padding(16.0f, 16.0f, 16.0f, 16.0f);
	window.set_content(box);
	// actions by name work the same way
	raven::action_bind("quit", [](Rid, const char*) { raven::quit(0); });
	window.register_shortcut(R_KEY_ESCAPE, 0, "quit");

	if (!test) {
		while (raven::wait_frame(-1.0)) {}
		raven::shutdown();
		return 0;
	}

	raven::draw_frame();
	EXPECT(window.is_valid() && greet.is_valid());
	EXPECT(greet.get_type() == R_WIDGET_BUTTON);
	EXPECT(raven::Widget(greet.get_parent()).get_child_count() == 3);
	name.set_text("Ada");
	float x, y, width, height;
	greet.get_rect(&x, &y, &width, &height);
	EXPECT(width > 0.0f && height > 0.0f);
	window.inject_mouse_move(x + width / 2, y + height / 2);
	window.inject_mouse_button(R_MOUSE_LEFT, R_TRUE);
	window.inject_mouse_button(R_MOUSE_LEFT, R_FALSE);
	EXPECT(clicks == 1);
	EXPECT(std::strcmp(title.get_text(), "Hello, Ada!") == 0);
	// set again: the first callable is released, the second one is called
	greet.set_clicked_callback([&](Rid) { clicks += 10; });
	window.inject_mouse_button(R_MOUSE_LEFT, R_TRUE);
	window.inject_mouse_button(R_MOUSE_LEFT, R_FALSE);
	EXPECT(clicks == 11);

	// widgets made by the type alone, a list filled by a lambda that returns text
	auto list = raven::ListView::create();
	std::string row;
	list.set_virtual_items(100, [&](Rid, int index) -> const char* {
		row = "row " + std::to_string(index);
		return row.c_str();
	});
	EXPECT(std::strcmp(list.get_item_text(42), "row 42") == 0);
	list.destroy();

	// documents
	auto document = raven::XmlDocument::create("VBox");
	auto root = raven::XmlNode(document.get_root());
	raven::XmlNode(root.add_child("Label", -1)).set_attribute("Text", "from a document");
	auto built = raven::Widget(document.create_widget());
	EXPECT(built.get_child_count() == 1);
	EXPECT(raven::Widget(built.get_child(0)).get_xml_node() == root.get_child(0));
	built.destroy();
	document.destroy();

	// free functions
	EXPECT(raven::xml_get_tag_count() > 30);
	raven::action_fire("quit", R_INVALID_ID);
	EXPECT(!raven::is_running());
	raven::shutdown();
	std::printf("c++ binding: ok\n");
	return 0;
}
