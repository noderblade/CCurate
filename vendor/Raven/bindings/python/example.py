"""
A window described in XML, driven from Python.

    RAVEN_LIBRARY=/path/to/libraven.so python3 example.py

`python3 example.py --test` runs without a window system and without a GPU,
clicks its own button and checks what happened: it is how the build of Raven
checks this binding.
"""

import sys

import raven as r

XML = """
<Window Title="Raven from Python" Width="360" Height="200">
    <VBox Spacing="10" Slot.Padding="16">
        <Label Name="title" Text="Hello from Python" TextSize="20"/>
        <TextInput Name="name" Placeholder="Your name"/>
        <HBox Spacing="8">
            <Button Name="greet" Text="Greet" Style="Primary" OnClicked="greet" Slot.Fill="1"/>
            <Button Text="Quit" OnClicked="quit"/>
        </HBox>
    </VBox>
</Window>
"""


def main():
    test = "--test" in sys.argv
    if test:
        desc = r.RInitDesc()
        desc.backend = r.R_BACKEND_NULL
        desc.headless = 1
        started = r.Rinit_ex(desc)
    else:
        started = r.Rinit(r.R_BACKEND_VULKAN)
    if not started:
        print("cannot start Raven")
        return 1

    errors = []
    r.Rerror_set_callback(lambda code, severity, detail: errors.append((code, detail)))

    window = r.Rwindow_create_from_xml(XML)
    title = r.Rwidget_find(window, "title")
    name = r.Rwidget_find(window, "name")
    clicks = []

    def greet(sender, action, user_data):
        clicks.append(action)
        r.Rlabel_set_text(title, "Hello, %s!" % (r.Rtext_input_get_text(name) or "you"))

    r.Raction_bind("greet", greet)
    r.Raction_bind("quit", lambda sender, action, user_data: r.Rquit(0))

    if not test:
        while r.Rwait_frame(-1.0):
            pass
        r.Rshutdown()
        return 0

    # ---- the binding, checked
    r.Rdraw_frame()
    assert r.Rwindow_get_title(window) == "Raven from Python"
    assert r.Rwidget_get_type(title) == r.R_WIDGET_LABEL
    r.Rtext_input_set_text(name, "Zoé")
    x, y, width, height = r.Rwidget_get_rect(r.Rwidget_find(window, "greet"))
    assert width > 0 and height > 0
    r.Rwindow_inject_mouse_move(window, x + width / 2, y + height / 2)
    r.Rwindow_inject_mouse_button(window, r.R_MOUSE_LEFT, 1)
    r.Rwindow_inject_mouse_button(window, r.R_MOUSE_LEFT, 0)
    assert clicks == ["greet"], clicks
    assert r.Rlabel_get_text(title) == "Hello, Zoé!", r.Rlabel_get_text(title)
    # values written through pointers come back as results
    assert r.Rwindow_get_size(window) == (360, 200)
    # structures
    stats = r.RMemoryStats()
    r.Rmemory_get_stats(stats)
    assert stats.allocation_count > 0
    # documents, and a mistake reported in a structure
    error = r.RXmlError()
    assert r.Rxml_document_parse("<A><B></A>", error) == r.R_INVALID_ID
    assert error.line == 1 and b"does not match" in error.message
    document = r.Rxml_document_parse("<VBox><Label Text='x'/></VBox>", None)
    root = r.Rxml_document_get_root(document)
    assert r.Rxml_node_get_tag(root) == "VBox" and r.Rxml_node_get_child_count(root) == 1
    assert r.Rxml_node_get_attribute(root, "Nope") is None
    r.Rxml_node_set_attribute(r.Rxml_node_get_child(root, 0), "Text", "é & <")
    assert 'Text="é &amp; &lt;"' in r.Rxml_document_to_string(document)
    # a callback that returns a text, and one that returns a number
    listed = r.Rwidget_create(r.R_WIDGET_LIST_VIEW)
    r.Rlist_view_set_virtual_items(listed, 3, lambda view, index, user_data: "row %d" % index)
    assert r.Rlist_view_get_item_text(listed, 2) == "row 2"
    r.Rwidget_destroy(listed)
    # an error of the caller reaches the callback as text
    r.Rwidget_set_tooltip(0x12345, "x")
    assert errors and errors[-1][0] == r.R_ERROR_INVALID_ID and "Rwidget_set_tooltip" in errors[-1][1], errors
    # files: bytes in, bytes out
    assert r.Rfile_write("raven_python_test.bin", b"a\x00b")
    assert r.Rfile_read("raven_python_test.bin") == b"a\x00b"
    import os
    os.remove("raven_python_test.bin")
    r.Raction_fire("quit", r.R_INVALID_ID)
    assert not r.Ris_running()
    r.Rshutdown()
    print("python binding: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
