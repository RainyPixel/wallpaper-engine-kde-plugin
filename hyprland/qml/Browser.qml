pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    // Set by runBrowse(); see main.cpp.
    required property var bridge
    required property var wallpapers

    property var selected: null
    property string message: ""

    readonly property color pageColor: "#f4f4f4"
    readonly property color cardColor: "#ffffff"
    readonly property color lineColor: "#d0d0d0"
    readonly property color textColor: "#202020"
    readonly property color mutedColor: "#707070"

    readonly property var types: {
        const seen = {};
        for (let i = 0; i < root.wallpapers.length; ++i)
            seen[root.wallpapers[i].type] = true;
        return ["all"].concat(Object.keys(seen).sort());
    }

    readonly property var filtered: {
        const needle = search.text.trim().toLowerCase();
        const type = typeFilter.currentText;
        return root.wallpapers.filter(function(item) {
            if (type !== "all" && item.type !== type)
                return false;
            return needle === "" || item.title.toLowerCase().indexOf(needle) !== -1;
        });
    }

    // Paths are unique and canonical; object identity is not stable across a
    // filter change, which rebuilds the delegate model.
    function isSelected(item) {
        return Boolean(root.selected) && Boolean(item) && root.selected.path === item.path;
    }

    function badgeText(item) {
        return item.supported ? item.type : item.type + " — not supported yet";
    }

    Rectangle {
        anchors.fill: parent
        color: root.pageColor
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: search

                Layout.fillWidth: true
                placeholderText: "Search by title"
            }

            ComboBox {
                id: typeFilter

                model: root.types
                implicitWidth: 160
                // Most Workshop items are scene wallpapers the host cannot play
                // yet, so open on the playable ones and leave the rest a click away.
                Component.onCompleted: {
                    const web = root.types.indexOf("web");
                    if(web !== -1) currentIndex = web;
                }
            }

            Label {
                text: root.filtered.length + " of " + root.wallpapers.length
                color: root.mutedColor
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: root.cardColor
                border.color: root.lineColor

                GridView {
                    id: grid

                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    // Nominal cell width, stretched so the columns fill the pane.
                    readonly property int minCellWidth: 220
                    cellWidth: Math.floor(width / Math.max(1, Math.floor(width / minCellWidth)))
                    cellHeight: 190
                    model: root.filtered
                    visible: count > 0

                    ScrollBar.vertical: ScrollBar {}

                    delegate: Item {
                        id: cell

                        required property var modelData

                        width: grid.cellWidth
                        height: grid.cellHeight
                        opacity: cell.modelData.supported ? 1.0 : 0.45

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 6
                            color: root.isSelected(cell.modelData) ? "#dce8f6" : "transparent"
                            border.color: root.isSelected(cell.modelData) ? "#5a8cc4" : root.lineColor
                            radius: 3

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 4

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 108
                                    color: "#e4e4e4"
                                    clip: true

                                    Image {
                                        id: thumb

                                        anchors.fill: parent
                                        source: cell.modelData.previewUrl
                                        asynchronous: true
                                        fillMode: Image.PreserveAspectCrop
                                        // Clamped, or a 4K preview per cell eats the memory.
                                        sourceSize.width: 320
                                        sourceSize.height: 200
                                        visible: status === Image.Ready
                                    }

                                    Label {
                                        anchors.centerIn: parent
                                        visible: !thumb.visible
                                        text: "no preview"
                                        color: root.mutedColor
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: cell.modelData.title
                                    color: root.textColor
                                    elide: Text.ElideRight
                                    maximumLineCount: 2
                                    wrapMode: Text.Wrap
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: root.badgeText(cell.modelData)
                                    color: root.mutedColor
                                    font.pointSize: 8
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                root.selected = cell.modelData;
                                root.message = "";
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 40
                    visible: grid.count === 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: root.mutedColor
                    text: root.wallpapers.length === 0
                          ? "No wallpapers found. Subscribe to some in Wallpaper Engine, or check that Steam is installed."
                          : "No wallpaper matches the search."
                }
            }

            Rectangle {
                Layout.preferredWidth: 320
                Layout.fillHeight: true
                color: root.cardColor
                border.color: root.lineColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 200
                        color: "#e4e4e4"
                        clip: true

                        // Only the detail pane animates; a grid of playing gifs is a
                        // memory problem.
                        AnimatedImage {
                            id: detailPreview

                            anchors.fill: parent
                            source: root.selected ? root.selected.previewUrl : ""
                            asynchronous: true
                            fillMode: Image.PreserveAspectFit
                            visible: status === AnimatedImage.Ready
                            onStatusChanged: playing = status === AnimatedImage.Ready
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: !detailPreview.visible
                            color: root.mutedColor
                            text: root.selected ? "no preview" : "select a wallpaper"
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.selected ? root.selected.title : ""
                        color: root.textColor
                        font.bold: true
                        wrapMode: Text.Wrap
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.selected ? root.badgeText(root.selected) : ""
                        color: root.mutedColor
                        wrapMode: Text.Wrap
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.selected ? root.selected.path : ""
                        color: root.mutedColor
                        font.pointSize: 8
                        elide: Text.ElideMiddle
                    }

                    Item {
                        Layout.fillHeight: true
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.message !== ""
                        text: root.message
                        color: root.textColor
                        wrapMode: Text.Wrap
                    }

                    Button {
                        Layout.fillWidth: true
                        text: "Apply"
                        enabled: root.selected !== null && root.selected.supported
                        onClicked: {
                            const error = root.bridge.apply(root.selected.path);
                            root.message = error === "" ? "Applied " + root.selected.title
                                                        : error;
                        }
                    }
                }
            }
        }
    }
}
