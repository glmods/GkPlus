#include "imgui-quickjs.h"

#include <imgui.h>

#include <cfloat>
#include <cstdint>
#include <vector>

#define countof(ARR) (sizeof(ARR) / sizeof(ARR[0]))

#define JS_ENUM_DEF(ENUM)                                                      \
  JS_OBJECT_DEF(#ENUM, ENUM, countof(ENUM), JS_PROP_ENUMERABLE)
#define JS_ENUM_ELEM(TYPE, ENUM)                                               \
  JS_PROP_INT32_DEF(#ENUM, ImGui##TYPE##_##ENUM, JS_PROP_ENUMERABLE)

namespace {
const JSCFunctionListEntry TableFlags[] = {
    JS_ENUM_ELEM(TableFlags, None),
    JS_ENUM_ELEM(TableFlags, Resizable),
    JS_ENUM_ELEM(TableFlags, Reorderable),
    JS_ENUM_ELEM(TableFlags, Hideable),
    JS_ENUM_ELEM(TableFlags, Sortable),
    JS_ENUM_ELEM(TableFlags, NoSavedSettings),
    JS_ENUM_ELEM(TableFlags, ContextMenuInBody),
    JS_ENUM_ELEM(TableFlags, RowBg),
    JS_ENUM_ELEM(TableFlags, BordersInnerH),
    JS_ENUM_ELEM(TableFlags, BordersOuterH),
    JS_ENUM_ELEM(TableFlags, BordersInnerV),
    JS_ENUM_ELEM(TableFlags, BordersOuterV),
    JS_ENUM_ELEM(TableFlags, BordersH),
    JS_ENUM_ELEM(TableFlags, BordersV),
    JS_ENUM_ELEM(TableFlags, BordersInner),
    JS_ENUM_ELEM(TableFlags, BordersOuter),
    JS_ENUM_ELEM(TableFlags, Borders),
    JS_ENUM_ELEM(TableFlags, NoBordersInBody),
    JS_ENUM_ELEM(TableFlags, NoBordersInBodyUntilResize),
    JS_ENUM_ELEM(TableFlags, SizingFixedFit),
    JS_ENUM_ELEM(TableFlags, SizingFixedSame),
    JS_ENUM_ELEM(TableFlags, SizingStretchProp),
    JS_ENUM_ELEM(TableFlags, SizingStretchSame),
    JS_ENUM_ELEM(TableFlags, NoHostExtendX),
    JS_ENUM_ELEM(TableFlags, NoHostExtendY),
    JS_ENUM_ELEM(TableFlags, NoKeepColumnsVisible),
    JS_ENUM_ELEM(TableFlags, PreciseWidths),
    JS_ENUM_ELEM(TableFlags, NoClip),
    JS_ENUM_ELEM(TableFlags, PadOuterX),
    JS_ENUM_ELEM(TableFlags, NoPadOuterX),
    JS_ENUM_ELEM(TableFlags, NoPadInnerX),
    JS_ENUM_ELEM(TableFlags, ScrollX),
    JS_ENUM_ELEM(TableFlags, ScrollY),
    JS_ENUM_ELEM(TableFlags, SortMulti),
    JS_ENUM_ELEM(TableFlags, SortTristate),
    JS_ENUM_ELEM(TableFlags, HighlightHoveredColumn),
};

const JSCFunctionListEntry TableColumnFlags[] = {
    JS_ENUM_ELEM(TableColumnFlags, None),
    JS_ENUM_ELEM(TableColumnFlags, Disabled),
    JS_ENUM_ELEM(TableColumnFlags, DefaultHide),
    JS_ENUM_ELEM(TableColumnFlags, DefaultSort),
    JS_ENUM_ELEM(TableColumnFlags, WidthStretch),
    JS_ENUM_ELEM(TableColumnFlags, WidthFixed),
    JS_ENUM_ELEM(TableColumnFlags, NoResize),
    JS_ENUM_ELEM(TableColumnFlags, NoReorder),
    JS_ENUM_ELEM(TableColumnFlags, NoHide),
    JS_ENUM_ELEM(TableColumnFlags, NoClip),
    JS_ENUM_ELEM(TableColumnFlags, NoSort),
    JS_ENUM_ELEM(TableColumnFlags, NoSortAscending),
    JS_ENUM_ELEM(TableColumnFlags, NoSortDescending),
    JS_ENUM_ELEM(TableColumnFlags, NoHeaderLabel),
    JS_ENUM_ELEM(TableColumnFlags, NoHeaderWidth),
    JS_ENUM_ELEM(TableColumnFlags, PreferSortAscending),
    JS_ENUM_ELEM(TableColumnFlags, PreferSortDescending),
    JS_ENUM_ELEM(TableColumnFlags, IndentEnable),
    JS_ENUM_ELEM(TableColumnFlags, IndentDisable),
    JS_ENUM_ELEM(TableColumnFlags, AngledHeader),
    JS_ENUM_ELEM(TableColumnFlags, IsEnabled),
    JS_ENUM_ELEM(TableColumnFlags, IsVisible),
    JS_ENUM_ELEM(TableColumnFlags, IsSorted),
    JS_ENUM_ELEM(TableColumnFlags, IsHovered),
};

const JSCFunctionListEntry TableRowFlags[] = {
    JS_ENUM_ELEM(TableRowFlags, None),
    JS_ENUM_ELEM(TableRowFlags, Headers),
};

const JSCFunctionListEntry TableBgTarget[] = {
    JS_ENUM_ELEM(TableBgTarget, None),
    JS_ENUM_ELEM(TableBgTarget, RowBg0),
    JS_ENUM_ELEM(TableBgTarget, RowBg1),
    JS_ENUM_ELEM(TableBgTarget, CellBg),
};

const JSCFunctionListEntry WindowFlags[] = {
    JS_ENUM_ELEM(WindowFlags, None),
    JS_ENUM_ELEM(WindowFlags, NoTitleBar),
    JS_ENUM_ELEM(WindowFlags, NoResize),
    JS_ENUM_ELEM(WindowFlags, NoMove),
    JS_ENUM_ELEM(WindowFlags, NoScrollbar),
    JS_ENUM_ELEM(WindowFlags, NoScrollWithMouse),
    JS_ENUM_ELEM(WindowFlags, NoCollapse),
    JS_ENUM_ELEM(WindowFlags, AlwaysAutoResize),
    JS_ENUM_ELEM(WindowFlags, NoBackground),
    JS_ENUM_ELEM(WindowFlags, NoSavedSettings),
    JS_ENUM_ELEM(WindowFlags, NoMouseInputs),
    JS_ENUM_ELEM(WindowFlags, MenuBar),
    JS_ENUM_ELEM(WindowFlags, HorizontalScrollbar),
    JS_ENUM_ELEM(WindowFlags, NoFocusOnAppearing),
    JS_ENUM_ELEM(WindowFlags, NoBringToFrontOnFocus),
    JS_ENUM_ELEM(WindowFlags, AlwaysVerticalScrollbar),
    JS_ENUM_ELEM(WindowFlags, AlwaysHorizontalScrollbar),
    JS_ENUM_ELEM(WindowFlags, NoNavInputs),
    JS_ENUM_ELEM(WindowFlags, NoNavFocus),
    JS_ENUM_ELEM(WindowFlags, UnsavedDocument),
    JS_ENUM_ELEM(WindowFlags, NoNav),
    JS_ENUM_ELEM(WindowFlags, NoDecoration),
    JS_ENUM_ELEM(WindowFlags, NoInputs),
};

const JSCFunctionListEntry ChildFlags[] = {
    JS_ENUM_ELEM(ChildFlags, None),
    JS_ENUM_ELEM(ChildFlags, Borders),
    JS_ENUM_ELEM(ChildFlags, AlwaysUseWindowPadding),
    JS_ENUM_ELEM(ChildFlags, ResizeX),
    JS_ENUM_ELEM(ChildFlags, ResizeY),
    JS_ENUM_ELEM(ChildFlags, AutoResizeX),
    JS_ENUM_ELEM(ChildFlags, AutoResizeY),
    JS_ENUM_ELEM(ChildFlags, AlwaysAutoResize),
    JS_ENUM_ELEM(ChildFlags, FrameStyle),
    JS_ENUM_ELEM(ChildFlags, NavFlattened),
};

const JSCFunctionListEntry ItemFlags[] = {
    JS_ENUM_ELEM(ItemFlags, None),
    JS_ENUM_ELEM(ItemFlags, NoTabStop),
    JS_ENUM_ELEM(ItemFlags, NoNav),
    JS_ENUM_ELEM(ItemFlags, NoNavDefaultFocus),
    JS_ENUM_ELEM(ItemFlags, ButtonRepeat),
    JS_ENUM_ELEM(ItemFlags, AutoClosePopups),
    JS_ENUM_ELEM(ItemFlags, AllowDuplicateId),
};

const JSCFunctionListEntry InputTextFlags[] = {
    JS_ENUM_ELEM(InputTextFlags, None),
    JS_ENUM_ELEM(InputTextFlags, CharsDecimal),
    JS_ENUM_ELEM(InputTextFlags, CharsHexadecimal),
    JS_ENUM_ELEM(InputTextFlags, CharsScientific),
    JS_ENUM_ELEM(InputTextFlags, CharsUppercase),
    JS_ENUM_ELEM(InputTextFlags, CharsNoBlank),
    JS_ENUM_ELEM(InputTextFlags, AllowTabInput),
    JS_ENUM_ELEM(InputTextFlags, EnterReturnsTrue),
    JS_ENUM_ELEM(InputTextFlags, EscapeClearsAll),
    JS_ENUM_ELEM(InputTextFlags, CtrlEnterForNewLine),
    JS_ENUM_ELEM(InputTextFlags, ReadOnly),
    JS_ENUM_ELEM(InputTextFlags, AlwaysOverwrite),
    JS_ENUM_ELEM(InputTextFlags, AutoSelectAll),
    JS_ENUM_ELEM(InputTextFlags, ParseEmptyRefVal),
    JS_ENUM_ELEM(InputTextFlags, DisplayEmptyRefVal),
    JS_ENUM_ELEM(InputTextFlags, NoHorizontalScroll),
    JS_ENUM_ELEM(InputTextFlags, NoUndoRedo),
    JS_ENUM_ELEM(InputTextFlags, ElideLeft),
    JS_ENUM_ELEM(InputTextFlags, CallbackCompletion),
    JS_ENUM_ELEM(InputTextFlags, CallbackHistory),
    JS_ENUM_ELEM(InputTextFlags, CallbackAlways),
    JS_ENUM_ELEM(InputTextFlags, CallbackCharFilter),
    JS_ENUM_ELEM(InputTextFlags, CallbackResize),
    JS_ENUM_ELEM(InputTextFlags, CallbackEdit),
};

const JSCFunctionListEntry SliderFlags[] = {
    JS_ENUM_ELEM(SliderFlags, None),
    JS_ENUM_ELEM(SliderFlags, Logarithmic),
    JS_ENUM_ELEM(SliderFlags, NoRoundToFormat),
    JS_ENUM_ELEM(SliderFlags, NoInput),
    JS_ENUM_ELEM(SliderFlags, WrapAround),
    JS_ENUM_ELEM(SliderFlags, ClampOnInput),
    JS_ENUM_ELEM(SliderFlags, ClampZeroRange),
    JS_ENUM_ELEM(SliderFlags, AlwaysClamp),
};

const JSCFunctionListEntry TreeNodeFlags[] = {
    JS_ENUM_ELEM(TreeNodeFlags, None),
    JS_ENUM_ELEM(TreeNodeFlags, Selected),
    JS_ENUM_ELEM(TreeNodeFlags, Framed),
    JS_ENUM_ELEM(TreeNodeFlags, AllowOverlap),
    JS_ENUM_ELEM(TreeNodeFlags, NoTreePushOnOpen),
    JS_ENUM_ELEM(TreeNodeFlags, NoAutoOpenOnLog),
    JS_ENUM_ELEM(TreeNodeFlags, DefaultOpen),
    JS_ENUM_ELEM(TreeNodeFlags, OpenOnDoubleClick),
    JS_ENUM_ELEM(TreeNodeFlags, OpenOnArrow),
    JS_ENUM_ELEM(TreeNodeFlags, Leaf),
    JS_ENUM_ELEM(TreeNodeFlags, Bullet),
    JS_ENUM_ELEM(TreeNodeFlags, FramePadding),
    JS_ENUM_ELEM(TreeNodeFlags, SpanAvailWidth),
    JS_ENUM_ELEM(TreeNodeFlags, SpanFullWidth),
    JS_ENUM_ELEM(TreeNodeFlags, SpanLabelWidth),
    JS_ENUM_ELEM(TreeNodeFlags, SpanAllColumns),
    JS_ENUM_ELEM(TreeNodeFlags, LabelSpanAllColumns),
    JS_ENUM_ELEM(TreeNodeFlags, NavLeftJumpsBackHere),
    JS_ENUM_ELEM(TreeNodeFlags, CollapsingHeader),
};

const JSCFunctionListEntry PopupFlags[] = {
    JS_ENUM_ELEM(PopupFlags, None),
    JS_ENUM_ELEM(PopupFlags, MouseButtonLeft),
    JS_ENUM_ELEM(PopupFlags, MouseButtonRight),
    JS_ENUM_ELEM(PopupFlags, MouseButtonMiddle),
    JS_ENUM_ELEM(PopupFlags, MouseButtonMask_),
    JS_ENUM_ELEM(PopupFlags, NoReopen),
    JS_ENUM_ELEM(PopupFlags, NoOpenOverExistingPopup),
    JS_ENUM_ELEM(PopupFlags, NoOpenOverItems),
    JS_ENUM_ELEM(PopupFlags, AnyPopupId),
    JS_ENUM_ELEM(PopupFlags, AnyPopupLevel),
    JS_ENUM_ELEM(PopupFlags, AnyPopup),
};

const JSCFunctionListEntry SelectableFlags[] = {
    JS_ENUM_ELEM(SelectableFlags, None),
    JS_ENUM_ELEM(SelectableFlags, NoAutoClosePopups),
    JS_ENUM_ELEM(SelectableFlags, SpanAllColumns),
    JS_ENUM_ELEM(SelectableFlags, AllowDoubleClick),
    JS_ENUM_ELEM(SelectableFlags, Disabled),
    JS_ENUM_ELEM(SelectableFlags, AllowOverlap),
    JS_ENUM_ELEM(SelectableFlags, Highlight),
};

const JSCFunctionListEntry ComboFlags[] = {
    JS_ENUM_ELEM(ComboFlags, None),
    JS_ENUM_ELEM(ComboFlags, PopupAlignLeft),
    JS_ENUM_ELEM(ComboFlags, HeightSmall),
    JS_ENUM_ELEM(ComboFlags, HeightRegular),
    JS_ENUM_ELEM(ComboFlags, HeightLarge),
    JS_ENUM_ELEM(ComboFlags, HeightLargest),
    JS_ENUM_ELEM(ComboFlags, NoArrowButton),
    JS_ENUM_ELEM(ComboFlags, NoPreview),
    JS_ENUM_ELEM(ComboFlags, WidthFitPreview),
    JS_ENUM_ELEM(ComboFlags, HeightMask_),
};

const JSCFunctionListEntry TabBarFlags[] = {
    JS_ENUM_ELEM(TabBarFlags, None),
    JS_ENUM_ELEM(TabBarFlags, Reorderable),
    JS_ENUM_ELEM(TabBarFlags, AutoSelectNewTabs),
    JS_ENUM_ELEM(TabBarFlags, TabListPopupButton),
    JS_ENUM_ELEM(TabBarFlags, NoCloseWithMiddleMouseButton),
    JS_ENUM_ELEM(TabBarFlags, NoTabListScrollingButtons),
    JS_ENUM_ELEM(TabBarFlags, NoTooltip),
    JS_ENUM_ELEM(TabBarFlags, DrawSelectedOverline),
    JS_ENUM_ELEM(TabBarFlags, FittingPolicyResizeDown),
    JS_ENUM_ELEM(TabBarFlags, FittingPolicyScroll),
    JS_ENUM_ELEM(TabBarFlags, FittingPolicyMask_),
    JS_ENUM_ELEM(TabBarFlags, FittingPolicyDefault_),
};

const JSCFunctionListEntry TabItemFlags[] = {
    JS_ENUM_ELEM(TabItemFlags, None),
    JS_ENUM_ELEM(TabItemFlags, UnsavedDocument),
    JS_ENUM_ELEM(TabItemFlags, SetSelected),
    JS_ENUM_ELEM(TabItemFlags, NoCloseWithMiddleMouseButton),
    JS_ENUM_ELEM(TabItemFlags, NoPushId),
    JS_ENUM_ELEM(TabItemFlags, NoTooltip),
    JS_ENUM_ELEM(TabItemFlags, NoReorder),
    JS_ENUM_ELEM(TabItemFlags, Leading),
    JS_ENUM_ELEM(TabItemFlags, Trailing),
    JS_ENUM_ELEM(TabItemFlags, NoAssumedClosure),
};

const JSCFunctionListEntry FocusedFlags[] = {
    JS_ENUM_ELEM(FocusedFlags, None),
    JS_ENUM_ELEM(FocusedFlags, ChildWindows),
    JS_ENUM_ELEM(FocusedFlags, RootWindow),
    JS_ENUM_ELEM(FocusedFlags, AnyWindow),
    JS_ENUM_ELEM(FocusedFlags, NoPopupHierarchy),
    JS_ENUM_ELEM(FocusedFlags, RootAndChildWindows),
};

const JSCFunctionListEntry HoveredFlags[] = {
    JS_ENUM_ELEM(HoveredFlags, None),
    JS_ENUM_ELEM(HoveredFlags, ChildWindows),
    JS_ENUM_ELEM(HoveredFlags, RootWindow),
    JS_ENUM_ELEM(HoveredFlags, AnyWindow),
    JS_ENUM_ELEM(HoveredFlags, NoPopupHierarchy),
    JS_ENUM_ELEM(HoveredFlags, AllowWhenBlockedByPopup),
    JS_ENUM_ELEM(HoveredFlags, AllowWhenBlockedByActiveItem),
    JS_ENUM_ELEM(HoveredFlags, AllowWhenOverlappedByItem),
    JS_ENUM_ELEM(HoveredFlags, AllowWhenOverlappedByWindow),
    JS_ENUM_ELEM(HoveredFlags, AllowWhenDisabled),
    JS_ENUM_ELEM(HoveredFlags, NoNavOverride),
    JS_ENUM_ELEM(HoveredFlags, AllowWhenOverlapped),
    JS_ENUM_ELEM(HoveredFlags, RectOnly),
    JS_ENUM_ELEM(HoveredFlags, RootAndChildWindows),
    JS_ENUM_ELEM(HoveredFlags, ForTooltip),
    JS_ENUM_ELEM(HoveredFlags, Stationary),
    JS_ENUM_ELEM(HoveredFlags, DelayNone),
    JS_ENUM_ELEM(HoveredFlags, DelayShort),
    JS_ENUM_ELEM(HoveredFlags, DelayNormal),
    JS_ENUM_ELEM(HoveredFlags, NoSharedDelay),
};

const JSCFunctionListEntry DragDropFlags[] = {
    JS_ENUM_ELEM(DragDropFlags, None),
    JS_ENUM_ELEM(DragDropFlags, SourceNoPreviewTooltip),
    JS_ENUM_ELEM(DragDropFlags, SourceNoDisableHover),
    JS_ENUM_ELEM(DragDropFlags, SourceNoHoldToOpenOthers),
    JS_ENUM_ELEM(DragDropFlags, SourceAllowNullID),
    JS_ENUM_ELEM(DragDropFlags, SourceExtern),
    JS_ENUM_ELEM(DragDropFlags, PayloadAutoExpire),
    JS_ENUM_ELEM(DragDropFlags, PayloadNoCrossContext),
    JS_ENUM_ELEM(DragDropFlags, PayloadNoCrossProcess),
    JS_ENUM_ELEM(DragDropFlags, AcceptBeforeDelivery),
    JS_ENUM_ELEM(DragDropFlags, AcceptNoDrawDefaultRect),
    JS_ENUM_ELEM(DragDropFlags, AcceptNoPreviewTooltip),
    JS_ENUM_ELEM(DragDropFlags, AcceptPeekOnly),
};

const JSCFunctionListEntry Dir[] = {
    JS_ENUM_ELEM(Dir, None), JS_ENUM_ELEM(Dir, Left), JS_ENUM_ELEM(Dir, Right),
    JS_ENUM_ELEM(Dir, Up),   JS_ENUM_ELEM(Dir, Down),
};

const JSCFunctionListEntry SortDirection[] = {
    JS_ENUM_ELEM(SortDirection, None),
    JS_ENUM_ELEM(SortDirection, Ascending),
    JS_ENUM_ELEM(SortDirection, Descending),
};
const JSCFunctionListEntry Key[] = {
    JS_ENUM_ELEM(Key, None),
    JS_ENUM_ELEM(Key, Tab),
    JS_ENUM_ELEM(Key, LeftArrow),
    JS_ENUM_ELEM(Key, RightArrow),
    JS_ENUM_ELEM(Key, UpArrow),
    JS_ENUM_ELEM(Key, DownArrow),
    JS_ENUM_ELEM(Key, PageUp),
    JS_ENUM_ELEM(Key, PageDown),
    JS_ENUM_ELEM(Key, Home),
    JS_ENUM_ELEM(Key, End),
    JS_ENUM_ELEM(Key, Insert),
    JS_ENUM_ELEM(Key, Delete),
    JS_ENUM_ELEM(Key, Backspace),
    JS_ENUM_ELEM(Key, Space),
    JS_ENUM_ELEM(Key, Enter),
    JS_ENUM_ELEM(Key, Escape),
    JS_ENUM_ELEM(Key, LeftCtrl),
    JS_ENUM_ELEM(Key, LeftShift),
    JS_ENUM_ELEM(Key, LeftAlt),
    JS_ENUM_ELEM(Key, LeftSuper),
    JS_ENUM_ELEM(Key, RightCtrl),
    JS_ENUM_ELEM(Key, RightShift),
    JS_ENUM_ELEM(Key, RightAlt),
    JS_ENUM_ELEM(Key, RightSuper),
    JS_ENUM_ELEM(Key, Menu),
    JS_ENUM_ELEM(Key, 0),
    JS_ENUM_ELEM(Key, 1),
    JS_ENUM_ELEM(Key, 2),
    JS_ENUM_ELEM(Key, 3),
    JS_ENUM_ELEM(Key, 4),
    JS_ENUM_ELEM(Key, 5),
    JS_ENUM_ELEM(Key, 6),
    JS_ENUM_ELEM(Key, 7),
    JS_ENUM_ELEM(Key, 8),
    JS_ENUM_ELEM(Key, 9),
    JS_ENUM_ELEM(Key, A),
    JS_ENUM_ELEM(Key, B),
    JS_ENUM_ELEM(Key, C),
    JS_ENUM_ELEM(Key, D),
    JS_ENUM_ELEM(Key, E),
    JS_ENUM_ELEM(Key, F),
    JS_ENUM_ELEM(Key, G),
    JS_ENUM_ELEM(Key, H),
    JS_ENUM_ELEM(Key, I),
    JS_ENUM_ELEM(Key, J),
    JS_ENUM_ELEM(Key, K),
    JS_ENUM_ELEM(Key, L),
    JS_ENUM_ELEM(Key, M),
    JS_ENUM_ELEM(Key, N),
    JS_ENUM_ELEM(Key, O),
    JS_ENUM_ELEM(Key, P),
    JS_ENUM_ELEM(Key, Q),
    JS_ENUM_ELEM(Key, R),
    JS_ENUM_ELEM(Key, S),
    JS_ENUM_ELEM(Key, T),
    JS_ENUM_ELEM(Key, U),
    JS_ENUM_ELEM(Key, V),
    JS_ENUM_ELEM(Key, W),
    JS_ENUM_ELEM(Key, X),
    JS_ENUM_ELEM(Key, Y),
    JS_ENUM_ELEM(Key, Z),
    JS_ENUM_ELEM(Key, F1),
    JS_ENUM_ELEM(Key, F2),
    JS_ENUM_ELEM(Key, F3),
    JS_ENUM_ELEM(Key, F4),
    JS_ENUM_ELEM(Key, F5),
    JS_ENUM_ELEM(Key, F6),
    JS_ENUM_ELEM(Key, F7),
    JS_ENUM_ELEM(Key, F8),
    JS_ENUM_ELEM(Key, F9),
    JS_ENUM_ELEM(Key, F10),
    JS_ENUM_ELEM(Key, F11),
    JS_ENUM_ELEM(Key, F12),
    JS_ENUM_ELEM(Key, F13),
    JS_ENUM_ELEM(Key, F14),
    JS_ENUM_ELEM(Key, F15),
    JS_ENUM_ELEM(Key, F16),
    JS_ENUM_ELEM(Key, F17),
    JS_ENUM_ELEM(Key, F18),
    JS_ENUM_ELEM(Key, F19),
    JS_ENUM_ELEM(Key, F20),
    JS_ENUM_ELEM(Key, F21),
    JS_ENUM_ELEM(Key, F22),
    JS_ENUM_ELEM(Key, F23),
    JS_ENUM_ELEM(Key, F24),
    JS_ENUM_ELEM(Key, Apostrophe),
    JS_ENUM_ELEM(Key, Comma),
    JS_ENUM_ELEM(Key, Minus),
    JS_ENUM_ELEM(Key, Period),
    JS_ENUM_ELEM(Key, Slash),
    JS_ENUM_ELEM(Key, Semicolon),
    JS_ENUM_ELEM(Key, Equal),
    JS_ENUM_ELEM(Key, LeftBracket),
    JS_ENUM_ELEM(Key, Backslash),
    JS_ENUM_ELEM(Key, RightBracket),
    JS_ENUM_ELEM(Key, GraveAccent),
    JS_ENUM_ELEM(Key, CapsLock),
    JS_ENUM_ELEM(Key, ScrollLock),
    JS_ENUM_ELEM(Key, NumLock),
    JS_ENUM_ELEM(Key, PrintScreen),
    JS_ENUM_ELEM(Key, Pause),
    JS_ENUM_ELEM(Key, Keypad0),
    JS_ENUM_ELEM(Key, Keypad1),
    JS_ENUM_ELEM(Key, Keypad2),
    JS_ENUM_ELEM(Key, Keypad3),
    JS_ENUM_ELEM(Key, Keypad4),
    JS_ENUM_ELEM(Key, Keypad5),
    JS_ENUM_ELEM(Key, Keypad6),
    JS_ENUM_ELEM(Key, Keypad7),
    JS_ENUM_ELEM(Key, Keypad8),
    JS_ENUM_ELEM(Key, Keypad9),
    JS_ENUM_ELEM(Key, KeypadDecimal),
    JS_ENUM_ELEM(Key, KeypadDivide),
    JS_ENUM_ELEM(Key, KeypadMultiply),
    JS_ENUM_ELEM(Key, KeypadSubtract),
    JS_ENUM_ELEM(Key, KeypadAdd),
    JS_ENUM_ELEM(Key, KeypadEnter),
    JS_ENUM_ELEM(Key, KeypadEqual),
    JS_ENUM_ELEM(Key, AppBack),
    JS_ENUM_ELEM(Key, AppForward),
    JS_ENUM_ELEM(Key, Oem102),
    JS_ENUM_ELEM(Key, GamepadStart),
    JS_ENUM_ELEM(Key, GamepadBack),
    JS_ENUM_ELEM(Key, GamepadFaceLeft),
    JS_ENUM_ELEM(Key, GamepadFaceRight),
    JS_ENUM_ELEM(Key, GamepadFaceUp),
    JS_ENUM_ELEM(Key, GamepadFaceDown),
    JS_ENUM_ELEM(Key, GamepadDpadLeft),
    JS_ENUM_ELEM(Key, GamepadDpadRight),
    JS_ENUM_ELEM(Key, GamepadDpadUp),
    JS_ENUM_ELEM(Key, GamepadDpadDown),
    JS_ENUM_ELEM(Key, GamepadL1),
    JS_ENUM_ELEM(Key, GamepadR1),
    JS_ENUM_ELEM(Key, GamepadL2),
    JS_ENUM_ELEM(Key, GamepadR2),
    JS_ENUM_ELEM(Key, GamepadL3),
    JS_ENUM_ELEM(Key, GamepadR3),
    JS_ENUM_ELEM(Key, GamepadLStickLeft),
    JS_ENUM_ELEM(Key, GamepadLStickRight),
    JS_ENUM_ELEM(Key, GamepadLStickUp),
    JS_ENUM_ELEM(Key, GamepadLStickDown),
    JS_ENUM_ELEM(Key, GamepadRStickLeft),
    JS_ENUM_ELEM(Key, GamepadRStickRight),
    JS_ENUM_ELEM(Key, GamepadRStickUp),
    JS_ENUM_ELEM(Key, GamepadRStickDown),
    JS_ENUM_ELEM(Key, MouseLeft),
    JS_ENUM_ELEM(Key, MouseRight),
    JS_ENUM_ELEM(Key, MouseMiddle),
    JS_ENUM_ELEM(Key, MouseX1),
    JS_ENUM_ELEM(Key, MouseX2),
    JS_ENUM_ELEM(Key, MouseWheelX),
    JS_ENUM_ELEM(Key, MouseWheelY),
};

const JSCFunctionListEntry Mod[] = {
    JS_ENUM_ELEM(Mod, None), JS_ENUM_ELEM(Mod, Ctrl),  JS_ENUM_ELEM(Mod, Shift),
    JS_ENUM_ELEM(Mod, Alt),  JS_ENUM_ELEM(Mod, Super), JS_ENUM_ELEM(Mod, Mask_),
};

const JSCFunctionListEntry MouseButton[] = {
    JS_ENUM_ELEM(MouseButton, Left),
    JS_ENUM_ELEM(MouseButton, Right),
    JS_ENUM_ELEM(MouseButton, Middle),
};

const JSCFunctionListEntry InputFlags[] = {
    JS_ENUM_ELEM(InputFlags, None),
    JS_ENUM_ELEM(InputFlags, Repeat),
    JS_ENUM_ELEM(InputFlags, RouteActive),
    JS_ENUM_ELEM(InputFlags, RouteFocused),
    JS_ENUM_ELEM(InputFlags, RouteGlobal),
    JS_ENUM_ELEM(InputFlags, RouteAlways),
    JS_ENUM_ELEM(InputFlags, RouteOverFocused),
    JS_ENUM_ELEM(InputFlags, RouteOverActive),
    JS_ENUM_ELEM(InputFlags, RouteUnlessBgFocused),
    JS_ENUM_ELEM(InputFlags, RouteFromRootWindow),
    JS_ENUM_ELEM(InputFlags, Tooltip),
};

const JSCFunctionListEntry ConfigFlags[] = {
    JS_ENUM_ELEM(ConfigFlags, None),
    JS_ENUM_ELEM(ConfigFlags, NavEnableKeyboard),
    JS_ENUM_ELEM(ConfigFlags, NavEnableGamepad),
    JS_ENUM_ELEM(ConfigFlags, NoMouse),
    JS_ENUM_ELEM(ConfigFlags, NoMouseCursorChange),
    JS_ENUM_ELEM(ConfigFlags, NoKeyboard),
    JS_ENUM_ELEM(ConfigFlags, IsSRGB),
    JS_ENUM_ELEM(ConfigFlags, IsTouchScreen),
};

const JSCFunctionListEntry Col[] = {
    JS_ENUM_ELEM(Col, Text),
    JS_ENUM_ELEM(Col, TextDisabled),
    JS_ENUM_ELEM(Col, WindowBg),
    JS_ENUM_ELEM(Col, ChildBg),
    JS_ENUM_ELEM(Col, PopupBg),
    JS_ENUM_ELEM(Col, Border),
    JS_ENUM_ELEM(Col, BorderShadow),
    JS_ENUM_ELEM(Col, FrameBg),
    JS_ENUM_ELEM(Col, FrameBgHovered),
    JS_ENUM_ELEM(Col, FrameBgActive),
    JS_ENUM_ELEM(Col, TitleBg),
    JS_ENUM_ELEM(Col, TitleBgActive),
    JS_ENUM_ELEM(Col, TitleBgCollapsed),
    JS_ENUM_ELEM(Col, MenuBarBg),
    JS_ENUM_ELEM(Col, ScrollbarBg),
    JS_ENUM_ELEM(Col, ScrollbarGrab),
    JS_ENUM_ELEM(Col, ScrollbarGrabHovered),
    JS_ENUM_ELEM(Col, ScrollbarGrabActive),
    JS_ENUM_ELEM(Col, CheckMark),
    JS_ENUM_ELEM(Col, SliderGrab),
    JS_ENUM_ELEM(Col, SliderGrabActive),
    JS_ENUM_ELEM(Col, Button),
    JS_ENUM_ELEM(Col, ButtonHovered),
    JS_ENUM_ELEM(Col, ButtonActive),
    JS_ENUM_ELEM(Col, Header),
    JS_ENUM_ELEM(Col, HeaderHovered),
    JS_ENUM_ELEM(Col, HeaderActive),
    JS_ENUM_ELEM(Col, Separator),
    JS_ENUM_ELEM(Col, SeparatorHovered),
    JS_ENUM_ELEM(Col, SeparatorActive),
    JS_ENUM_ELEM(Col, ResizeGrip),
    JS_ENUM_ELEM(Col, ResizeGripHovered),
    JS_ENUM_ELEM(Col, ResizeGripActive),
    JS_ENUM_ELEM(Col, TabHovered),
    JS_ENUM_ELEM(Col, Tab),
    JS_ENUM_ELEM(Col, TabSelected),
    JS_ENUM_ELEM(Col, TabSelectedOverline),
    JS_ENUM_ELEM(Col, TabDimmed),
    JS_ENUM_ELEM(Col, TabDimmedSelected),
    JS_ENUM_ELEM(Col, TabDimmedSelectedOverline),
    JS_ENUM_ELEM(Col, PlotLines),
    JS_ENUM_ELEM(Col, PlotLinesHovered),
    JS_ENUM_ELEM(Col, PlotHistogram),
    JS_ENUM_ELEM(Col, PlotHistogramHovered),
    JS_ENUM_ELEM(Col, TableHeaderBg),
    JS_ENUM_ELEM(Col, TableBorderStrong),
    JS_ENUM_ELEM(Col, TableBorderLight),
    JS_ENUM_ELEM(Col, TableRowBg),
    JS_ENUM_ELEM(Col, TableRowBgAlt),
    JS_ENUM_ELEM(Col, TextLink),
    JS_ENUM_ELEM(Col, TextSelectedBg),
    JS_ENUM_ELEM(Col, DragDropTarget),
    JS_ENUM_ELEM(Col, NavCursor),
    JS_ENUM_ELEM(Col, NavWindowingHighlight),
    JS_ENUM_ELEM(Col, NavWindowingDimBg),
    JS_ENUM_ELEM(Col, ModalWindowDimBg),
};

const JSCFunctionListEntry ButtonFlags[] = {
    JS_ENUM_ELEM(ButtonFlags, None),
    JS_ENUM_ELEM(ButtonFlags, MouseButtonLeft),
    JS_ENUM_ELEM(ButtonFlags, MouseButtonRight),
    JS_ENUM_ELEM(ButtonFlags, MouseButtonMiddle),
};

const JSCFunctionListEntry ColorEditFlags[] = {
    JS_ENUM_ELEM(ColorEditFlags, None),
    JS_ENUM_ELEM(ColorEditFlags, NoAlpha),
    JS_ENUM_ELEM(ColorEditFlags, NoPicker),
    JS_ENUM_ELEM(ColorEditFlags, NoOptions),
    JS_ENUM_ELEM(ColorEditFlags, NoSmallPreview),
    JS_ENUM_ELEM(ColorEditFlags, NoInputs),
    JS_ENUM_ELEM(ColorEditFlags, NoTooltip),
    JS_ENUM_ELEM(ColorEditFlags, NoLabel),
    JS_ENUM_ELEM(ColorEditFlags, NoSidePreview),
    JS_ENUM_ELEM(ColorEditFlags, NoDragDrop),
    JS_ENUM_ELEM(ColorEditFlags, NoBorder),
    JS_ENUM_ELEM(ColorEditFlags, AlphaOpaque),
    JS_ENUM_ELEM(ColorEditFlags, AlphaNoBg),
    JS_ENUM_ELEM(ColorEditFlags, AlphaPreviewHalf),
    JS_ENUM_ELEM(ColorEditFlags, AlphaBar),
    JS_ENUM_ELEM(ColorEditFlags, HDR),
    JS_ENUM_ELEM(ColorEditFlags, DisplayRGB),
    JS_ENUM_ELEM(ColorEditFlags, DisplayHSV),
    JS_ENUM_ELEM(ColorEditFlags, DisplayHex),
    JS_ENUM_ELEM(ColorEditFlags, Uint8),
    JS_ENUM_ELEM(ColorEditFlags, Float),
    JS_ENUM_ELEM(ColorEditFlags, PickerHueBar),
    JS_ENUM_ELEM(ColorEditFlags, PickerHueWheel),
    JS_ENUM_ELEM(ColorEditFlags, InputRGB),
    JS_ENUM_ELEM(ColorEditFlags, InputHSV),
    JS_ENUM_ELEM(ColorEditFlags, DefaultOptions_),
};

// Helper function to extract ImVec2 from JavaScript object or return default
static ImVec2 js_to_ImVec2(JSContext *ctx, JSValueConst val,
                           const ImVec2 &def = ImVec2(0, 0)) {
  if (JS_IsObject(val)) {
    ImVec2 result = def;
    JSValue x = JS_GetPropertyStr(ctx, val, "x");
    JSValue y = JS_GetPropertyStr(ctx, val, "y");

    if (JS_IsNumber(x)) {
      double dx;
      JS_ToFloat64(ctx, &dx, x);
      result.x = static_cast<float>(dx);
    }
    if (JS_IsNumber(y)) {
      double dy;
      JS_ToFloat64(ctx, &dy, y);
      result.y = static_cast<float>(dy);
    }

    JS_FreeValue(ctx, x);
    JS_FreeValue(ctx, y);
    return result;
  }
  return def;
}

// Helper function to extract ImVec4 from JavaScript object
static ImVec4 js_to_ImVec4(JSContext *ctx, JSValueConst val,
                           const ImVec4 &def = ImVec4(0, 0, 0, 0)) {
  if (JS_IsObject(val)) {
    ImVec4 result = def;
    JSValue x = JS_GetPropertyStr(ctx, val, "x");
    JSValue y = JS_GetPropertyStr(ctx, val, "y");
    JSValue z = JS_GetPropertyStr(ctx, val, "z");
    JSValue w = JS_GetPropertyStr(ctx, val, "w");

    if (JS_IsNumber(x)) {
      double dx;
      JS_ToFloat64(ctx, &dx, x);
      result.x = static_cast<float>(dx);
    }
    if (JS_IsNumber(y)) {
      double dy;
      JS_ToFloat64(ctx, &dy, y);
      result.y = static_cast<float>(dy);
    }
    if (JS_IsNumber(z)) {
      double dz;
      JS_ToFloat64(ctx, &dz, z);
      result.z = static_cast<float>(dz);
    }
    if (JS_IsNumber(w)) {
      double dw;
      JS_ToFloat64(ctx, &dw, w);
      result.w = static_cast<float>(dw);
    }

    JS_FreeValue(ctx, x);
    JS_FreeValue(ctx, y);
    JS_FreeValue(ctx, z);
    JS_FreeValue(ctx, w);
    return result;
  }
  return def;
}

// Helper function to extract ImVec2 from a JavaScript object
static bool js_to_ImVec2(JSContext *ctx, JSValueConst val, ImVec2 &vec) {
  if (!JS_IsObject(val)) {
    return false;
  }

  JSValue x_val = JS_GetPropertyStr(ctx, val, "x");
  JSValue y_val = JS_GetPropertyStr(ctx, val, "y");

  double x, y;
  bool success = (JS_ToFloat64(ctx, &x, x_val) == 0) &&
                 (JS_ToFloat64(ctx, &y, y_val) == 0);

  JS_FreeValue(ctx, x_val);
  JS_FreeValue(ctx, y_val);

  if (success) {
    vec.x = static_cast<float>(x);
    vec.y = static_cast<float>(y);
  }

  return success;
}

// Helper function to create a JavaScript object from ImVec2
static JSValue ImVec2_to_js(JSContext *ctx, const ImVec2 &vec) {
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, vec.x));
  JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, vec.y));
  return obj;
}

// Helper function to convert ImVec4 to JavaScript object
static JSValue ImVec4_to_js(JSContext *ctx, const ImVec4 &vec) {
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, vec.x));
  JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, vec.y));
  JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, vec.z));
  JS_SetPropertyStr(ctx, obj, "w", JS_NewFloat64(ctx, vec.w));
  return obj;
}

// ============================================================================
// Text Widgets
// ============================================================================

// ImGui::Text(const char* text)
static JSValue js_ImGui_Text(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "Text: expected 1 argument (text)");
  }

  const char *text = JS_ToCString(ctx, argv[0]);
  if (!text) {
    return JS_EXCEPTION;
  }

  ImGui::Text("%s", text);

  JS_FreeCString(ctx, text);
  return JS_UNDEFINED;
}

// ImGui::TextColored(const ImVec4& col, const char* text)
static JSValue js_ImGui_TextColored(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx,
                             "TextColored: expected 2 arguments (col, text)");
  }

  ImVec4 col = js_to_ImVec4(ctx, argv[0]);

  const char *text = JS_ToCString(ctx, argv[1]);
  if (!text) {
    return JS_EXCEPTION;
  }

  ImGui::TextColored(col, "%s", text);

  JS_FreeCString(ctx, text);
  return JS_UNDEFINED;
}

// ImGui::TextDisabled(const char* text)
static JSValue js_ImGui_TextDisabled(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "TextDisabled: expected 1 argument (text)");
  }

  const char *text = JS_ToCString(ctx, argv[0]);
  if (!text) {
    return JS_EXCEPTION;
  }

  ImGui::TextDisabled("%s", text);

  JS_FreeCString(ctx, text);
  return JS_UNDEFINED;
}

// ImGui::TextWrapped(const char* text)
static JSValue js_ImGui_TextWrapped(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "TextWrapped: expected 1 argument (text)");
  }

  const char *text = JS_ToCString(ctx, argv[0]);
  if (!text) {
    return JS_EXCEPTION;
  }

  ImGui::TextWrapped("%s", text);

  JS_FreeCString(ctx, text);
  return JS_UNDEFINED;
}

// ImGui::LabelText(const char* label, const char* text)
static JSValue js_ImGui_LabelText(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx,
                             "LabelText: expected 2 arguments (label, text)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  const char *text = JS_ToCString(ctx, argv[1]);
  if (!text) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  ImGui::LabelText(label, "%s", text);

  JS_FreeCString(ctx, label);
  JS_FreeCString(ctx, text);
  return JS_UNDEFINED;
}

// ImGui::BulletText(const char* text)
static JSValue js_ImGui_BulletText(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "BulletText: expected 1 argument (text)");
  }

  const char *text = JS_ToCString(ctx, argv[0]);
  if (!text) {
    return JS_EXCEPTION;
  }

  ImGui::BulletText("%s", text);

  JS_FreeCString(ctx, text);
  return JS_UNDEFINED;
}

// ImGui::SeparatorText(const char* label)
static JSValue js_ImGui_SeparatorText(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "SeparatorText: expected 1 argument (label)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  ImGui::SeparatorText(label);

  JS_FreeCString(ctx, label);
  return JS_UNDEFINED;
}

// ============================================================================
// Button Widgets
// ============================================================================

// ImGui::Button(const char* label, const ImVec2& size = ImVec2(0,0))
// JS: Button(label, options?) where options = {size: {x, y}}
static JSValue js_ImGui_Button(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "Button: expected at least 1 argument (label)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  ImVec2 size(0, 0);

  // Handle optional parameters
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue size_val = JS_GetPropertyStr(ctx, argv[1], "size");
    if (!JS_IsUndefined(size_val)) {
      size = js_to_ImVec2(ctx, size_val);
    }
    JS_FreeValue(ctx, size_val);
  }

  bool result = ImGui::Button(label, size);

  JS_FreeCString(ctx, label);
  return JS_NewBool(ctx, result);
}

// ImGui::SmallButton(const char* label)
static JSValue js_ImGui_SmallButton(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "SmallButton: expected 1 argument (label)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  bool result = ImGui::SmallButton(label);

  JS_FreeCString(ctx, label);
  return JS_NewBool(ctx, result);
}

// ImGui::InvisibleButton(const char* str_id, const ImVec2& size,
// ImGuiButtonFlags flags = 0) JS: InvisibleButton(str_id, size, options?) where
// options = {flags}
static JSValue js_ImGui_InvisibleButton(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "InvisibleButton: expected at least 2 arguments (str_id, size)");
  }

  const char *str_id = JS_ToCString(ctx, argv[0]);
  if (!str_id) {
    return JS_EXCEPTION;
  }

  ImVec2 size = js_to_ImVec2(ctx, argv[1]);

  ImGuiButtonFlags flags = 0;
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t flags_int;
      if (JS_ToInt32(ctx, &flags_int, flags_val) == 0) {
        flags = static_cast<ImGuiButtonFlags>(flags_int);
      }
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool result = ImGui::InvisibleButton(str_id, size, flags);

  JS_FreeCString(ctx, str_id);
  return JS_NewBool(ctx, result);
}

// ImGui::ArrowButton(const char* str_id, ImGuiDir dir)
static JSValue js_ImGui_ArrowButton(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx,
                             "ArrowButton: expected 2 arguments (str_id, dir)");
  }

  const char *str_id = JS_ToCString(ctx, argv[0]);
  if (!str_id) {
    return JS_EXCEPTION;
  }

  int32_t dir_int;
  if (JS_ToInt32(ctx, &dir_int, argv[1]) != 0) {
    JS_FreeCString(ctx, str_id);
    return JS_EXCEPTION;
  }

  bool result = ImGui::ArrowButton(str_id, static_cast<ImGuiDir>(dir_int));

  JS_FreeCString(ctx, str_id);
  return JS_NewBool(ctx, result);
}

// ImGui::Checkbox(const char* label, bool* v)
// JS: Returns {changed: bool, value: bool}
static JSValue js_ImGui_Checkbox(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx,
                             "Checkbox: expected 2 arguments (label, value)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  bool value = JS_ToBool(ctx, argv[1]);
  bool changed = ImGui::Checkbox(label, &value);

  JS_FreeCString(ctx, label);

  // Return object with {changed, value}
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "value", JS_NewBool(ctx, value));

  return result;
}

// ImGui::RadioButton(const char* label, bool active)
static JSValue js_ImGui_RadioButton(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "RadioButton: expected 2 arguments (label, active)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  bool active = JS_ToBool(ctx, argv[1]);
  bool result = ImGui::RadioButton(label, active);

  JS_FreeCString(ctx, label);
  return JS_NewBool(ctx, result);
}

// ImGui::RadioButton(const char* label, int* v, int v_button)
// JS: RadioButtonEx(label, value, v_button) - Returns {changed: bool, value:
// int}
static JSValue js_ImGui_RadioButtonEx(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  if (argc < 3) {
    return JS_ThrowTypeError(
        ctx, "RadioButtonEx: expected 3 arguments (label, value, v_button)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  int32_t value;
  if (JS_ToInt32(ctx, &value, argv[1]) != 0) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  int32_t v_button;
  if (JS_ToInt32(ctx, &v_button, argv[2]) != 0) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  bool changed = ImGui::RadioButton(label, &value, v_button);

  JS_FreeCString(ctx, label);

  // Return object with {changed, value}
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "value", JS_NewInt32(ctx, value));

  return result;
}

// ============================================================================
// Other Basic Widgets
// ============================================================================

// ImGui::ProgressBar(float fraction, const ImVec2& size_arg =
// ImVec2(-FLT_MIN,0), const char* overlay = NULL) JS: ProgressBar(fraction,
// options?) where options = {size: {x, y}, overlay: string}
static JSValue js_ImGui_ProgressBar(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "ProgressBar: expected at least 1 argument (fraction)");
  }

  double fraction;
  if (JS_ToFloat64(ctx, &fraction, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  ImVec2 size_arg(-FLT_MIN, 0);
  const char *overlay = nullptr;

  // Handle optional parameters
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue size_val = JS_GetPropertyStr(ctx, argv[1], "size");
    if (!JS_IsUndefined(size_val)) {
      size_arg = js_to_ImVec2(ctx, size_val, ImVec2(-FLT_MIN, 0));
    }
    JS_FreeValue(ctx, size_val);

    JSValue overlay_val = JS_GetPropertyStr(ctx, argv[1], "overlay");
    if (!JS_IsUndefined(overlay_val) && !JS_IsNull(overlay_val)) {
      overlay = JS_ToCString(ctx, overlay_val);
    }
    JS_FreeValue(ctx, overlay_val);
  }

  ImGui::ProgressBar(static_cast<float>(fraction), size_arg, overlay);

  if (overlay) {
    JS_FreeCString(ctx, overlay);
  }

  return JS_UNDEFINED;
}

// ImGui::Bullet()
static JSValue js_ImGui_Bullet(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  ImGui::Bullet();
  return JS_UNDEFINED;
}

// ImGui::Image(ImTextureID user_texture_id, const ImVec2& image_size, ...)
// JS: Image(texture_id, image_size, options?)
// options = {uv0: {x, y}, uv1: {x, y}, tint_col: {x, y, z, w}, border_col: {x,
// y, z, w}}
static JSValue js_ImGui_Image(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "Image: expected at least 2 arguments (texture_id, image_size)");
  }

  // Get texture ID - can be a number or pointer
  int64_t texture_id_int;
  if (JS_ToInt64(ctx, &texture_id_int, argv[0]) != 0) {
    return JS_EXCEPTION;
  }
  ImTextureID texture_id = (ImTextureID)(uintptr_t)texture_id_int;

  ImVec2 image_size = js_to_ImVec2(ctx, argv[1]);

  ImVec2 uv0(0, 0);
  ImVec2 uv1(1, 1);
  ImVec4 tint_col(1, 1, 1, 1);
  ImVec4 border_col(0, 0, 0, 0);

  // Handle optional parameters
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue uv0_val = JS_GetPropertyStr(ctx, argv[2], "uv0");
    if (!JS_IsUndefined(uv0_val)) {
      uv0 = js_to_ImVec2(ctx, uv0_val, ImVec2(0, 0));
    }
    JS_FreeValue(ctx, uv0_val);

    JSValue uv1_val = JS_GetPropertyStr(ctx, argv[2], "uv1");
    if (!JS_IsUndefined(uv1_val)) {
      uv1 = js_to_ImVec2(ctx, uv1_val, ImVec2(1, 1));
    }
    JS_FreeValue(ctx, uv1_val);

    JSValue tint_val = JS_GetPropertyStr(ctx, argv[2], "tint_col");
    if (!JS_IsUndefined(tint_val)) {
      tint_col = js_to_ImVec4(ctx, tint_val, ImVec4(1, 1, 1, 1));
    }
    JS_FreeValue(ctx, tint_val);

    JSValue border_val = JS_GetPropertyStr(ctx, argv[2], "border_col");
    if (!JS_IsUndefined(border_val)) {
      border_col = js_to_ImVec4(ctx, border_val, ImVec4(0, 0, 0, 0));
    }
    JS_FreeValue(ctx, border_val);
  }

  ImGui::Image(texture_id, image_size, uv0, uv1, tint_col, border_col);

  return JS_UNDEFINED;
}

// ImGui::ImageButton(const char* str_id, ImTextureID user_texture_id, const
// ImVec2& image_size, ...) JS: ImageButton(str_id, texture_id, image_size,
// options?) options = {uv0: {x, y}, uv1: {x, y}, bg_col: {x, y, z, w},
// tint_col: {x, y, z, w}}
static JSValue js_ImGui_ImageButton(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 3) {
    return JS_ThrowTypeError(ctx, "ImageButton: expected at least 3 arguments "
                                  "(str_id, texture_id, image_size)");
  }

  const char *str_id = JS_ToCString(ctx, argv[0]);
  if (!str_id) {
    return JS_EXCEPTION;
  }

  // Get texture ID
  int64_t texture_id_int;
  if (JS_ToInt64(ctx, &texture_id_int, argv[1]) != 0) {
    JS_FreeCString(ctx, str_id);
    return JS_EXCEPTION;
  }
  ImTextureID texture_id = (ImTextureID)(uintptr_t)texture_id_int;

  ImVec2 image_size = js_to_ImVec2(ctx, argv[2]);

  ImVec2 uv0(0, 0);
  ImVec2 uv1(1, 1);
  ImVec4 bg_col(0, 0, 0, 0);
  ImVec4 tint_col(1, 1, 1, 1);

  // Handle optional parameters
  if (argc >= 4 && JS_IsObject(argv[3])) {
    JSValue uv0_val = JS_GetPropertyStr(ctx, argv[3], "uv0");
    if (!JS_IsUndefined(uv0_val)) {
      uv0 = js_to_ImVec2(ctx, uv0_val, ImVec2(0, 0));
    }
    JS_FreeValue(ctx, uv0_val);

    JSValue uv1_val = JS_GetPropertyStr(ctx, argv[3], "uv1");
    if (!JS_IsUndefined(uv1_val)) {
      uv1 = js_to_ImVec2(ctx, uv1_val, ImVec2(1, 1));
    }
    JS_FreeValue(ctx, uv1_val);

    JSValue bg_val = JS_GetPropertyStr(ctx, argv[3], "bg_col");
    if (!JS_IsUndefined(bg_val)) {
      bg_col = js_to_ImVec4(ctx, bg_val, ImVec4(0, 0, 0, 0));
    }
    JS_FreeValue(ctx, bg_val);

    JSValue tint_val = JS_GetPropertyStr(ctx, argv[3], "tint_col");
    if (!JS_IsUndefined(tint_val)) {
      tint_col = js_to_ImVec4(ctx, tint_val, ImVec4(1, 1, 1, 1));
    }
    JS_FreeValue(ctx, tint_val);
  }

  bool result = ImGui::ImageButton(str_id, texture_id, image_size, uv0, uv1,
                                   bg_col, tint_col);

  JS_FreeCString(ctx, str_id);
  return JS_NewBool(ctx, result);
}

// ============================================================================
// Helper Functions for Array Conversions
// ============================================================================

// Helper function to extract float array from JavaScript array
static bool js_get_float_array(JSContext *ctx, JSValueConst arr, float *out,
                               int count) {
  for (int i = 0; i < count; i++) {
    JSValue val = JS_GetPropertyUint32(ctx, arr, i);
    double d;
    if (JS_ToFloat64(ctx, &d, val)) {
      JS_FreeValue(ctx, val);
      return false;
    }
    out[i] = static_cast<float>(d);
    JS_FreeValue(ctx, val);
  }
  return true;
}

// Helper function to extract int array from JavaScript array
static bool js_get_int_array(JSContext *ctx, JSValueConst arr, int *out,
                             int count) {
  for (int i = 0; i < count; i++) {
    JSValue val = JS_GetPropertyUint32(ctx, arr, i);
    int32_t v;
    if (JS_ToInt32(ctx, &v, val)) {
      JS_FreeValue(ctx, val);
      return false;
    }
    out[i] = v;
    JS_FreeValue(ctx, val);
  }
  return true;
}

// Helper function to create JavaScript array from float array
static JSValue js_new_float_array(JSContext *ctx, const float *arr, int count) {
  JSValue result = JS_NewArray(ctx);
  for (int i = 0; i < count; i++) {
    JS_SetPropertyUint32(ctx, result, i, JS_NewFloat64(ctx, arr[i]));
  }
  return result;
}

// Helper function to create JavaScript array from int array
static JSValue js_new_int_array(JSContext *ctx, const int *arr, int count) {
  JSValue result = JS_NewArray(ctx);
  for (int i = 0; i < count; i++) {
    JS_SetPropertyUint32(ctx, result, i, JS_NewInt32(ctx, arr[i]));
  }
  return result;
}

// ============================================================================
// Plot Widgets
// ============================================================================

// ImGui::PlotLines(const char* label, const float* values, int values_count,
// int values_offset = 0, const char* overlay_text = NULL, float scale_min =
// FLT_MAX, float scale_max = FLT_MAX, ImVec2 graph_size = ImVec2(0, 0), int
// stride = sizeof(float))
// JS: PlotLines(label, values, options?) - an array of numbers as a line graph.
//
// No values_offset: a JS caller rotates its own array, and a ring index that
// does not match the array it was taken from plots the wrong samples silently.
// An omitted scale_min/scale_max stays FLT_MAX, which is ImGui's "fit to the
// data" rather than a bound of zero.
static JSValue js_ImGui_PlotLines(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx,
                             "PlotLines requires 2 arguments (label, values)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  if (!JS_IsArray(argv[1])) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx, "PlotLines: values must be an array");
  }

  JSValue length_val = JS_GetPropertyStr(ctx, argv[1], "length");
  int32_t count = 0;
  if (JS_ToInt32(ctx, &count, length_val) != 0) {
    JS_FreeValue(ctx, length_val);
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }
  JS_FreeValue(ctx, length_val);
  if (count < 0) {
    count = 0;
  }

  std::vector<float> values(static_cast<size_t>(count), 0.0f);
  if (count > 0 && !js_get_float_array(ctx, argv[1], values.data(), count)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx, "PlotLines: values must be numbers");
  }

  float scale_min = FLT_MAX;
  float scale_max = FLT_MAX;
  ImVec2 graph_size(0, 0);
  const char *overlay = nullptr;

  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue scale_min_val = JS_GetPropertyStr(ctx, argv[2], "scale_min");
    if (!JS_IsUndefined(scale_min_val) && !JS_IsNull(scale_min_val)) {
      double temp;
      if (JS_ToFloat64(ctx, &temp, scale_min_val) == 0) {
        scale_min = static_cast<float>(temp);
      }
    }
    JS_FreeValue(ctx, scale_min_val);

    JSValue scale_max_val = JS_GetPropertyStr(ctx, argv[2], "scale_max");
    if (!JS_IsUndefined(scale_max_val) && !JS_IsNull(scale_max_val)) {
      double temp;
      if (JS_ToFloat64(ctx, &temp, scale_max_val) == 0) {
        scale_max = static_cast<float>(temp);
      }
    }
    JS_FreeValue(ctx, scale_max_val);

    JSValue size_val = JS_GetPropertyStr(ctx, argv[2], "size");
    if (!JS_IsUndefined(size_val)) {
      graph_size = js_to_ImVec2(ctx, size_val, ImVec2(0, 0));
    }
    JS_FreeValue(ctx, size_val);

    JSValue overlay_val = JS_GetPropertyStr(ctx, argv[2], "overlay");
    if (!JS_IsUndefined(overlay_val) && !JS_IsNull(overlay_val)) {
      overlay = JS_ToCString(ctx, overlay_val);
    }
    JS_FreeValue(ctx, overlay_val);
  }

  ImGui::PlotLines(label, values.data(), count, 0, overlay, scale_min,
                   scale_max, graph_size);

  if (overlay) {
    JS_FreeCString(ctx, overlay);
  }
  JS_FreeCString(ctx, label);
  return JS_UNDEFINED;
}

// ImGui::PlotHistogram(const char* label, const float* values, int
// values_count, int values_offset = 0, const char* overlay_text = NULL, float
// scale_min = FLT_MAX, float scale_max = FLT_MAX, ImVec2 graph_size = ImVec2(0,
// 0), int stride = sizeof(float))
// JS: PlotHistogram(label, values, options?) - the same data as bars.
static JSValue js_ImGui_PlotHistogram(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "PlotHistogram requires 2 arguments (label, values)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  if (!JS_IsArray(argv[1])) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx, "PlotHistogram: values must be an array");
  }

  JSValue length_val = JS_GetPropertyStr(ctx, argv[1], "length");
  int32_t count = 0;
  if (JS_ToInt32(ctx, &count, length_val) != 0) {
    JS_FreeValue(ctx, length_val);
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }
  JS_FreeValue(ctx, length_val);
  if (count < 0) {
    count = 0;
  }

  std::vector<float> values(static_cast<size_t>(count), 0.0f);
  if (count > 0 && !js_get_float_array(ctx, argv[1], values.data(), count)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx, "PlotHistogram: values must be numbers");
  }

  float scale_min = FLT_MAX;
  float scale_max = FLT_MAX;
  ImVec2 graph_size(0, 0);
  const char *overlay = nullptr;

  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue scale_min_val = JS_GetPropertyStr(ctx, argv[2], "scale_min");
    if (!JS_IsUndefined(scale_min_val) && !JS_IsNull(scale_min_val)) {
      double temp;
      if (JS_ToFloat64(ctx, &temp, scale_min_val) == 0) {
        scale_min = static_cast<float>(temp);
      }
    }
    JS_FreeValue(ctx, scale_min_val);

    JSValue scale_max_val = JS_GetPropertyStr(ctx, argv[2], "scale_max");
    if (!JS_IsUndefined(scale_max_val) && !JS_IsNull(scale_max_val)) {
      double temp;
      if (JS_ToFloat64(ctx, &temp, scale_max_val) == 0) {
        scale_max = static_cast<float>(temp);
      }
    }
    JS_FreeValue(ctx, scale_max_val);

    JSValue size_val = JS_GetPropertyStr(ctx, argv[2], "size");
    if (!JS_IsUndefined(size_val)) {
      graph_size = js_to_ImVec2(ctx, size_val, ImVec2(0, 0));
    }
    JS_FreeValue(ctx, size_val);

    JSValue overlay_val = JS_GetPropertyStr(ctx, argv[2], "overlay");
    if (!JS_IsUndefined(overlay_val) && !JS_IsNull(overlay_val)) {
      overlay = JS_ToCString(ctx, overlay_val);
    }
    JS_FreeValue(ctx, overlay_val);
  }

  ImGui::PlotHistogram(label, values.data(), count, 0, overlay, scale_min,
                       scale_max, graph_size);

  if (overlay) {
    JS_FreeCString(ctx, overlay);
  }
  JS_FreeCString(ctx, label);
  return JS_UNDEFINED;
}

// ============================================================================
// Input Text Widgets
// ============================================================================

// ImGui::InputText(const char* label, char* buf, size_t buf_size,
// ImGuiInputTextFlags flags = 0) JS: InputText(label, text, options?) - Returns
// {changed: bool, text: string}
static JSValue js_ImGui_InputText(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "InputText: expected at least 2 arguments (label, text)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  const char *text_in = JS_ToCString(ctx, argv[1]);
  if (!text_in) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  ImGuiInputTextFlags flags = 0;
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiInputTextFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  char buffer[1024];
  strncpy(buffer, text_in, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  bool changed = ImGui::InputText(label, buffer, sizeof(buffer), flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "text", JS_NewString(ctx, buffer));

  JS_FreeCString(ctx, label);
  JS_FreeCString(ctx, text_in);

  return result;
}

// ImGui::InputTextMultiline(const char* label, char* buf, size_t buf_size,
// const ImVec2& size = ImVec2(0,0), ImGuiInputTextFlags flags = 0) JS:
// InputTextMultiline(label, text, options?) - Returns {changed: bool, text:
// string}
static JSValue js_ImGui_InputTextMultiline(JSContext *ctx,
                                           JSValueConst this_val, int argc,
                                           JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "InputTextMultiline: expected at least 2 arguments (label, text)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  const char *text_in = JS_ToCString(ctx, argv[1]);
  if (!text_in) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  ImVec2 size(0, 0);
  ImGuiInputTextFlags flags = 0;
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue size_val = JS_GetPropertyStr(ctx, argv[2], "size");
    if (!JS_IsUndefined(size_val)) {
      size = js_to_ImVec2(ctx, size_val);
    }
    JS_FreeValue(ctx, size_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiInputTextFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  char buffer[4096];
  strncpy(buffer, text_in, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  bool changed =
      ImGui::InputTextMultiline(label, buffer, sizeof(buffer), size, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "text", JS_NewString(ctx, buffer));

  JS_FreeCString(ctx, label);
  JS_FreeCString(ctx, text_in);

  return result;
}

// ImGui::InputTextWithHint(const char* label, const char* hint, char* buf,
// size_t buf_size, ImGuiInputTextFlags flags = 0) JS: InputTextWithHint(label,
// hint, text, options?) - Returns {changed: bool, text: string}
static JSValue js_ImGui_InputTextWithHint(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  if (argc < 3) {
    return JS_ThrowTypeError(
        ctx,
        "InputTextWithHint: expected at least 3 arguments (label, hint, text)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  const char *hint = JS_ToCString(ctx, argv[1]);
  if (!hint) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  const char *text_in = JS_ToCString(ctx, argv[2]);
  if (!text_in) {
    JS_FreeCString(ctx, label);
    JS_FreeCString(ctx, hint);
    return JS_EXCEPTION;
  }

  ImGuiInputTextFlags flags = 0;
  if (argc >= 4 && JS_IsObject(argv[3])) {
    JSValue flags_val = JS_GetPropertyStr(ctx, argv[3], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiInputTextFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  char buffer[1024];
  strncpy(buffer, text_in, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = '\0';

  bool changed =
      ImGui::InputTextWithHint(label, hint, buffer, sizeof(buffer), flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "text", JS_NewString(ctx, buffer));

  JS_FreeCString(ctx, label);
  JS_FreeCString(ctx, hint);
  JS_FreeCString(ctx, text_in);

  return result;
}

// ============================================================================
// Input Number Widgets
// ============================================================================

// ImGui::InputFloat(const char* label, float* v, float step = 0.0f, float
// step_fast = 0.0f, const char* format = "%.3f", ImGuiInputTextFlags flags = 0)
// JS: InputFloat(label, value, options?) - Returns {changed: bool, value:
// number}
static JSValue js_ImGui_InputFloat(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "InputFloat: expected at least 2 arguments (label, value)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  double v_d;
  if (JS_ToFloat64(ctx, &v_d, argv[1])) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }
  float v = static_cast<float>(v_d);

  float step = 0.0f;
  float step_fast = 0.0f;
  const char *format = "%.3f";
  ImGuiInputTextFlags flags = 0;
  bool free_format = false;

  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue step_val = JS_GetPropertyStr(ctx, argv[2], "step");
    if (!JS_IsUndefined(step_val)) {
      double s;
      JS_ToFloat64(ctx, &s, step_val);
      step = static_cast<float>(s);
    }
    JS_FreeValue(ctx, step_val);

    JSValue step_fast_val = JS_GetPropertyStr(ctx, argv[2], "stepFast");
    if (!JS_IsUndefined(step_fast_val)) {
      double sf;
      JS_ToFloat64(ctx, &sf, step_fast_val);
      step_fast = static_cast<float>(sf);
    }
    JS_FreeValue(ctx, step_fast_val);

    JSValue format_val = JS_GetPropertyStr(ctx, argv[2], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiInputTextFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::InputFloat(label, &v, step, step_fast, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "value", JS_NewFloat64(ctx, v));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ImGui::InputFloat2(const char* label, float v[2], const char* format =
// "%.3f", ImGuiInputTextFlags flags = 0) JS: InputFloat2(label, values,
// options?) - Returns {changed: bool, values: [number, number]}
static JSValue js_ImGui_InputFloat2(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "InputFloat2: expected at least 2 arguments (label, values)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  float v[2];
  if (!js_get_float_array(ctx, argv[1], v, 2)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx,
                             "Second argument must be an array of 2 numbers");
  }

  const char *format = "%.3f";
  ImGuiInputTextFlags flags = 0;
  bool free_format = false;

  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue format_val = JS_GetPropertyStr(ctx, argv[2], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiInputTextFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::InputFloat2(label, v, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "values", js_new_float_array(ctx, v, 2));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ImGui::InputFloat3(const char* label, float v[3], const char* format =
// "%.3f", ImGuiInputTextFlags flags = 0) JS: InputFloat3(label, values,
// options?) - Returns {changed: bool, values: [number, number, number]}
static JSValue js_ImGui_InputFloat3(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "InputFloat3: expected at least 2 arguments (label, values)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  float v[3];
  if (!js_get_float_array(ctx, argv[1], v, 3)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx,
                             "Second argument must be an array of 3 numbers");
  }

  const char *format = "%.3f";
  ImGuiInputTextFlags flags = 0;
  bool free_format = false;

  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue format_val = JS_GetPropertyStr(ctx, argv[2], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiInputTextFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::InputFloat3(label, v, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "values", js_new_float_array(ctx, v, 3));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ImGui::InputFloat4(const char* label, float v[4], const char* format =
// "%.3f", ImGuiInputTextFlags flags = 0) JS: InputFloat4(label, values,
// options?) - Returns {changed: bool, values: [number, number, number, number]}
static JSValue js_ImGui_InputFloat4(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "InputFloat4: expected at least 2 arguments (label, values)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  float v[4];
  if (!js_get_float_array(ctx, argv[1], v, 4)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx,
                             "Second argument must be an array of 4 numbers");
  }

  const char *format = "%.3f";
  ImGuiInputTextFlags flags = 0;
  bool free_format = false;

  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue format_val = JS_GetPropertyStr(ctx, argv[2], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiInputTextFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::InputFloat4(label, v, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "values", js_new_float_array(ctx, v, 4));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ImGui::InputInt(const char* label, int* v, int step = 1, int step_fast = 100,
// ImGuiInputTextFlags flags = 0) JS: InputInt(label, value, options?) - Returns
// {changed: bool, value: number}
static JSValue js_ImGui_InputInt(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "InputInt: expected at least 2 arguments (label, value)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  int32_t v;
  if (JS_ToInt32(ctx, &v, argv[1])) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  int step = 1;
  int step_fast = 100;
  ImGuiInputTextFlags flags = 0;

  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue step_val = JS_GetPropertyStr(ctx, argv[2], "step");
    if (!JS_IsUndefined(step_val)) {
      int32_t s;
      JS_ToInt32(ctx, &s, step_val);
      step = s;
    }
    JS_FreeValue(ctx, step_val);

    JSValue step_fast_val = JS_GetPropertyStr(ctx, argv[2], "stepFast");
    if (!JS_IsUndefined(step_fast_val)) {
      int32_t sf;
      JS_ToInt32(ctx, &sf, step_fast_val);
      step_fast = sf;
    }
    JS_FreeValue(ctx, step_fast_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiInputTextFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::InputInt(label, &v, step, step_fast, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "value", JS_NewInt32(ctx, v));

  JS_FreeCString(ctx, label);

  return result;
}

// ImGui::InputInt2(const char* label, int v[2], ImGuiInputTextFlags flags = 0)
// JS: InputInt2(label, values, options?) - Returns {changed: bool, values:
// [number, number]}
static JSValue js_ImGui_InputInt2(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "InputInt2: expected at least 2 arguments (label, values)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  int v[2];
  if (!js_get_int_array(ctx, argv[1], v, 2)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx,
                             "Second argument must be an array of 2 integers");
  }

  ImGuiInputTextFlags flags = 0;
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiInputTextFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::InputInt2(label, v, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "values", js_new_int_array(ctx, v, 2));

  JS_FreeCString(ctx, label);

  return result;
}

// ImGui::InputInt3(const char* label, int v[3], ImGuiInputTextFlags flags = 0)
// JS: InputInt3(label, values, options?) - Returns {changed: bool, values:
// [number, number, number]}
static JSValue js_ImGui_InputInt3(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "InputInt3: expected at least 2 arguments (label, values)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  int v[3];
  if (!js_get_int_array(ctx, argv[1], v, 3)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx,
                             "Second argument must be an array of 3 integers");
  }

  ImGuiInputTextFlags flags = 0;
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiInputTextFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::InputInt3(label, v, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "values", js_new_int_array(ctx, v, 3));

  JS_FreeCString(ctx, label);

  return result;
}

// ImGui::InputInt4(const char* label, int v[4], ImGuiInputTextFlags flags = 0)
// JS: InputInt4(label, values, options?) - Returns {changed: bool, values:
// [number, number, number, number]}
static JSValue js_ImGui_InputInt4(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "InputInt4: expected at least 2 arguments (label, values)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  int v[4];
  if (!js_get_int_array(ctx, argv[1], v, 4)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx,
                             "Second argument must be an array of 4 integers");
  }

  ImGuiInputTextFlags flags = 0;
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiInputTextFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::InputInt4(label, v, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "values", js_new_int_array(ctx, v, 4));

  JS_FreeCString(ctx, label);

  return result;
}

// ============================================================================
// Slider Widgets
// ============================================================================

// ImGui::SliderFloat(const char* label, float* v, float v_min, float v_max,
// const char* format = "%.3f", ImGuiSliderFlags flags = 0) JS:
// SliderFloat(label, value, min, max, options?) - Returns {changed: bool,
// value: number}
static JSValue js_ImGui_SliderFloat(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 4) {
    return JS_ThrowTypeError(
        ctx,
        "SliderFloat: expected at least 4 arguments (label, value, min, max)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  double v_d, min_d, max_d;
  if (JS_ToFloat64(ctx, &v_d, argv[1]) || JS_ToFloat64(ctx, &min_d, argv[2]) ||
      JS_ToFloat64(ctx, &max_d, argv[3])) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  float v = static_cast<float>(v_d);
  float v_min = static_cast<float>(min_d);
  float v_max = static_cast<float>(max_d);

  const char *format = "%.3f";
  ImGuiSliderFlags flags = 0;
  bool free_format = false;

  if (argc >= 5 && JS_IsObject(argv[4])) {
    JSValue format_val = JS_GetPropertyStr(ctx, argv[4], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[4], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiSliderFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::SliderFloat(label, &v, v_min, v_max, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "value", JS_NewFloat64(ctx, v));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ImGui::SliderFloat2(const char* label, float v[2], float v_min, float v_max,
// const char* format = "%.3f", ImGuiSliderFlags flags = 0) JS:
// SliderFloat2(label, values, min, max, options?) - Returns {changed: bool,
// values: [number, number]}
static JSValue js_ImGui_SliderFloat2(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 4) {
    return JS_ThrowTypeError(ctx, "SliderFloat2: expected at least 4 arguments "
                                  "(label, values, min, max)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  float v[2];
  if (!js_get_float_array(ctx, argv[1], v, 2)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx,
                             "Second argument must be an array of 2 numbers");
  }

  double min_d, max_d;
  if (JS_ToFloat64(ctx, &min_d, argv[2]) ||
      JS_ToFloat64(ctx, &max_d, argv[3])) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  float v_min = static_cast<float>(min_d);
  float v_max = static_cast<float>(max_d);

  const char *format = "%.3f";
  ImGuiSliderFlags flags = 0;
  bool free_format = false;

  if (argc >= 5 && JS_IsObject(argv[4])) {
    JSValue format_val = JS_GetPropertyStr(ctx, argv[4], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[4], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiSliderFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::SliderFloat2(label, v, v_min, v_max, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "values", js_new_float_array(ctx, v, 2));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ImGui::SliderFloat3(const char* label, float v[3], float v_min, float v_max,
// const char* format = "%.3f", ImGuiSliderFlags flags = 0) JS:
// SliderFloat3(label, values, min, max, options?) - Returns {changed: bool,
// values: [number, number, number]}
static JSValue js_ImGui_SliderFloat3(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 4) {
    return JS_ThrowTypeError(ctx, "SliderFloat3: expected at least 4 arguments "
                                  "(label, values, min, max)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  float v[3];
  if (!js_get_float_array(ctx, argv[1], v, 3)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx,
                             "Second argument must be an array of 3 numbers");
  }

  double min_d, max_d;
  if (JS_ToFloat64(ctx, &min_d, argv[2]) ||
      JS_ToFloat64(ctx, &max_d, argv[3])) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  float v_min = static_cast<float>(min_d);
  float v_max = static_cast<float>(max_d);

  const char *format = "%.3f";
  ImGuiSliderFlags flags = 0;
  bool free_format = false;

  if (argc >= 5 && JS_IsObject(argv[4])) {
    JSValue format_val = JS_GetPropertyStr(ctx, argv[4], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[4], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiSliderFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::SliderFloat3(label, v, v_min, v_max, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "values", js_new_float_array(ctx, v, 3));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ImGui::SliderFloat4(const char* label, float v[4], float v_min, float v_max,
// const char* format = "%.3f", ImGuiSliderFlags flags = 0) JS:
// SliderFloat4(label, values, min, max, options?) - Returns {changed: bool,
// values: [number, number, number, number]}
static JSValue js_ImGui_SliderFloat4(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 4) {
    return JS_ThrowTypeError(ctx, "SliderFloat4: expected at least 4 arguments "
                                  "(label, values, min, max)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  float v[4];
  if (!js_get_float_array(ctx, argv[1], v, 4)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx,
                             "Second argument must be an array of 4 numbers");
  }

  double min_d, max_d;
  if (JS_ToFloat64(ctx, &min_d, argv[2]) ||
      JS_ToFloat64(ctx, &max_d, argv[3])) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  float v_min = static_cast<float>(min_d);
  float v_max = static_cast<float>(max_d);

  const char *format = "%.3f";
  ImGuiSliderFlags flags = 0;
  bool free_format = false;

  if (argc >= 5 && JS_IsObject(argv[4])) {
    JSValue format_val = JS_GetPropertyStr(ctx, argv[4], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[4], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiSliderFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::SliderFloat4(label, v, v_min, v_max, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "values", js_new_float_array(ctx, v, 4));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ImGui::SliderInt(const char* label, int* v, int v_min, int v_max, const char*
// format = "%d", ImGuiSliderFlags flags = 0) JS: SliderInt(label, value, min,
// max, options?) - Returns {changed: bool, value: number}
static JSValue js_ImGui_SliderInt(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 4) {
    return JS_ThrowTypeError(
        ctx,
        "SliderInt: expected at least 4 arguments (label, value, min, max)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  int32_t v, v_min, v_max;
  if (JS_ToInt32(ctx, &v, argv[1]) || JS_ToInt32(ctx, &v_min, argv[2]) ||
      JS_ToInt32(ctx, &v_max, argv[3])) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  const char *format = "%d";
  ImGuiSliderFlags flags = 0;
  bool free_format = false;

  if (argc >= 5 && JS_IsObject(argv[4])) {
    JSValue format_val = JS_GetPropertyStr(ctx, argv[4], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[4], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiSliderFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::SliderInt(label, &v, v_min, v_max, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "value", JS_NewInt32(ctx, v));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ImGui::SliderAngle(const char* label, float* v_rad, float v_degrees_min =
// -360.0f, float v_degrees_max = +360.0f, const char* format = "%.0f deg",
// ImGuiSliderFlags flags = 0) JS: SliderAngle(label, v_rad, options?) - Returns
// {changed: bool, value: number}
static JSValue js_ImGui_SliderAngle(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "SliderAngle: expected at least 2 arguments (label, value)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  double v_d;
  if (JS_ToFloat64(ctx, &v_d, argv[1])) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }
  float v_rad = static_cast<float>(v_d);

  float v_degrees_min = -360.0f;
  float v_degrees_max = 360.0f;
  const char *format = "%.0f deg";
  ImGuiSliderFlags flags = 0;
  bool free_format = false;

  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue min_val = JS_GetPropertyStr(ctx, argv[2], "minDegrees");
    if (!JS_IsUndefined(min_val)) {
      double min_d;
      JS_ToFloat64(ctx, &min_d, min_val);
      v_degrees_min = static_cast<float>(min_d);
    }
    JS_FreeValue(ctx, min_val);

    JSValue max_val = JS_GetPropertyStr(ctx, argv[2], "maxDegrees");
    if (!JS_IsUndefined(max_val)) {
      double max_d;
      JS_ToFloat64(ctx, &max_d, max_val);
      v_degrees_max = static_cast<float>(max_d);
    }
    JS_FreeValue(ctx, max_val);

    JSValue format_val = JS_GetPropertyStr(ctx, argv[2], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiSliderFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed = ImGui::SliderAngle(label, &v_rad, v_degrees_min, v_degrees_max,
                                    format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "value", JS_NewFloat64(ctx, v_rad));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ============================================================================
// Drag Widgets
// ============================================================================

// ImGui::DragFloat(const char* label, float* v, float v_speed = 1.0f, float
// v_min = 0.0f, float v_max = 0.0f, const char* format = "%.3f",
// ImGuiSliderFlags flags = 0) JS: DragFloat(label, value, options?) - Returns
// {changed: bool, value: number}
static JSValue js_ImGui_DragFloat(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "DragFloat: expected at least 2 arguments (label, value)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  double v_d;
  if (JS_ToFloat64(ctx, &v_d, argv[1])) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }
  float v = static_cast<float>(v_d);

  float v_speed = 1.0f;
  float v_min = 0.0f;
  float v_max = 0.0f;
  const char *format = "%.3f";
  ImGuiSliderFlags flags = 0;
  bool free_format = false;

  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue speed_val = JS_GetPropertyStr(ctx, argv[2], "speed");
    if (!JS_IsUndefined(speed_val)) {
      double s;
      JS_ToFloat64(ctx, &s, speed_val);
      v_speed = static_cast<float>(s);
    }
    JS_FreeValue(ctx, speed_val);

    JSValue min_val = JS_GetPropertyStr(ctx, argv[2], "min");
    if (!JS_IsUndefined(min_val)) {
      double min_d;
      JS_ToFloat64(ctx, &min_d, min_val);
      v_min = static_cast<float>(min_d);
    }
    JS_FreeValue(ctx, min_val);

    JSValue max_val = JS_GetPropertyStr(ctx, argv[2], "max");
    if (!JS_IsUndefined(max_val)) {
      double max_d;
      JS_ToFloat64(ctx, &max_d, max_val);
      v_max = static_cast<float>(max_d);
    }
    JS_FreeValue(ctx, max_val);

    JSValue format_val = JS_GetPropertyStr(ctx, argv[2], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiSliderFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed =
      ImGui::DragFloat(label, &v, v_speed, v_min, v_max, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "value", JS_NewFloat64(ctx, v));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ImGui::DragFloat2(const char* label, float v[2], float v_speed = 1.0f, float
// v_min = 0.0f, float v_max = 0.0f, const char* format = "%.3f",
// ImGuiSliderFlags flags = 0) JS: DragFloat2(label, values, options?) - Returns
// {changed: bool, values: [number, number]}
static JSValue js_ImGui_DragFloat2(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "DragFloat2: expected at least 2 arguments (label, values)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  float v[2];
  if (!js_get_float_array(ctx, argv[1], v, 2)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx,
                             "Second argument must be an array of 2 numbers");
  }

  float v_speed = 1.0f;
  float v_min = 0.0f;
  float v_max = 0.0f;
  const char *format = "%.3f";
  ImGuiSliderFlags flags = 0;
  bool free_format = false;

  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue speed_val = JS_GetPropertyStr(ctx, argv[2], "speed");
    if (!JS_IsUndefined(speed_val)) {
      double s;
      JS_ToFloat64(ctx, &s, speed_val);
      v_speed = static_cast<float>(s);
    }
    JS_FreeValue(ctx, speed_val);

    JSValue min_val = JS_GetPropertyStr(ctx, argv[2], "min");
    if (!JS_IsUndefined(min_val)) {
      double min_d;
      JS_ToFloat64(ctx, &min_d, min_val);
      v_min = static_cast<float>(min_d);
    }
    JS_FreeValue(ctx, min_val);

    JSValue max_val = JS_GetPropertyStr(ctx, argv[2], "max");
    if (!JS_IsUndefined(max_val)) {
      double max_d;
      JS_ToFloat64(ctx, &max_d, max_val);
      v_max = static_cast<float>(max_d);
    }
    JS_FreeValue(ctx, max_val);

    JSValue format_val = JS_GetPropertyStr(ctx, argv[2], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiSliderFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed =
      ImGui::DragFloat2(label, v, v_speed, v_min, v_max, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "values", js_new_float_array(ctx, v, 2));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ImGui::DragInt(const char* label, int* v, float v_speed = 1.0f, int v_min =
// 0, int v_max = 0, const char* format = "%d", ImGuiSliderFlags flags = 0) JS:
// DragInt(label, value, options?) - Returns {changed: bool, value: number}
static JSValue js_ImGui_DragInt(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "DragInt: expected at least 2 arguments (label, value)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label)
    return JS_EXCEPTION;

  int32_t v;
  if (JS_ToInt32(ctx, &v, argv[1])) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  float v_speed = 1.0f;
  int v_min = 0;
  int v_max = 0;
  const char *format = "%d";
  ImGuiSliderFlags flags = 0;
  bool free_format = false;

  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue speed_val = JS_GetPropertyStr(ctx, argv[2], "speed");
    if (!JS_IsUndefined(speed_val)) {
      double s;
      JS_ToFloat64(ctx, &s, speed_val);
      v_speed = static_cast<float>(s);
    }
    JS_FreeValue(ctx, speed_val);

    JSValue min_val = JS_GetPropertyStr(ctx, argv[2], "min");
    if (!JS_IsUndefined(min_val)) {
      int32_t min_i;
      JS_ToInt32(ctx, &min_i, min_val);
      v_min = min_i;
    }
    JS_FreeValue(ctx, min_val);

    JSValue max_val = JS_GetPropertyStr(ctx, argv[2], "max");
    if (!JS_IsUndefined(max_val)) {
      int32_t max_i;
      JS_ToInt32(ctx, &max_i, max_val);
      v_max = max_i;
    }
    JS_FreeValue(ctx, max_val);

    JSValue format_val = JS_GetPropertyStr(ctx, argv[2], "format");
    if (!JS_IsUndefined(format_val)) {
      format = JS_ToCString(ctx, format_val);
      free_format = true;
    }
    JS_FreeValue(ctx, format_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t f;
      JS_ToInt32(ctx, &f, flags_val);
      flags = static_cast<ImGuiSliderFlags>(f);
    }
    JS_FreeValue(ctx, flags_val);
  }

  bool changed =
      ImGui::DragInt(label, &v, v_speed, v_min, v_max, format, flags);

  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "value", JS_NewInt32(ctx, v));

  JS_FreeCString(ctx, label);
  if (free_format)
    JS_FreeCString(ctx, format);

  return result;
}

// ============================================================================
// Helper Functions for Color Array Conversions
// ============================================================================

// Helper function to extract float array from JavaScript array (colors)
static bool js_get_color_array(JSContext *ctx, JSValueConst arr, float *out,
                               int count) {
  if (!JS_IsArray(arr)) {
    return false;
  }

  for (int i = 0; i < count; i++) {
    JSValue val = JS_GetPropertyUint32(ctx, arr, i);
    double d;
    if (JS_ToFloat64(ctx, &d, val)) {
      JS_FreeValue(ctx, val);
      return false;
    }
    out[i] = static_cast<float>(d);
    JS_FreeValue(ctx, val);
  }
  return true;
}

// Helper function to create JavaScript array from float array (colors)
static JSValue js_new_color_array(JSContext *ctx, const float *arr, int count) {
  JSValue result = JS_NewArray(ctx);
  for (int i = 0; i < count; i++) {
    JS_SetPropertyUint32(ctx, result, i, JS_NewFloat64(ctx, arr[i]));
  }
  return result;
}

// ============================================================================
// Color Editor Widgets
// ============================================================================

// ImGui::ColorEdit3(const char* label, float col[3], ImGuiColorEditFlags flags
// = 0) JS: ColorEdit3(label, color, flags?) - Returns {changed: bool, color:
// [r, g, b]} color: array [r, g, b] with values 0.0-1.0
static JSValue js_ImGui_ColorEdit3(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "ColorEdit3: expected at least 2 arguments (label, color)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  float col[3];
  if (!js_get_color_array(ctx, argv[1], col, 3)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(
        ctx, "ColorEdit3: color must be an array of 3 numbers [r, g, b]");
  }

  int32_t flags = 0;
  if (argc >= 3) {
    if (JS_ToInt32(ctx, &flags, argv[2]) != 0) {
      JS_FreeCString(ctx, label);
      return JS_EXCEPTION;
    }
  }

  bool changed = ImGui::ColorEdit3(label, col, flags);
  JS_FreeCString(ctx, label);

  // Return object with {changed, color}
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "color", js_new_color_array(ctx, col, 3));

  return result;
}

// ImGui::ColorEdit4(const char* label, float col[4], ImGuiColorEditFlags flags
// = 0) JS: ColorEdit4(label, color, flags?) - Returns {changed: bool, color:
// [r, g, b, a]} color: array [r, g, b, a] with values 0.0-1.0
static JSValue js_ImGui_ColorEdit4(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "ColorEdit4: expected at least 2 arguments (label, color)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  float col[4];
  if (!js_get_color_array(ctx, argv[1], col, 4)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(
        ctx, "ColorEdit4: color must be an array of 4 numbers [r, g, b, a]");
  }

  int32_t flags = 0;
  if (argc >= 3) {
    if (JS_ToInt32(ctx, &flags, argv[2]) != 0) {
      JS_FreeCString(ctx, label);
      return JS_EXCEPTION;
    }
  }

  bool changed = ImGui::ColorEdit4(label, col, flags);
  JS_FreeCString(ctx, label);

  // Return object with {changed, color}
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "color", js_new_color_array(ctx, col, 4));

  return result;
}

// ============================================================================
// Color Picker Widgets
// ============================================================================

// ImGui::ColorPicker3(const char* label, float col[3], ImGuiColorEditFlags
// flags = 0) JS: ColorPicker3(label, color, flags?) - Returns {changed: bool,
// color: [r, g, b]} color: array [r, g, b] with values 0.0-1.0
static JSValue js_ImGui_ColorPicker3(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "ColorPicker3: expected at least 2 arguments (label, color)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  float col[3];
  if (!js_get_color_array(ctx, argv[1], col, 3)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(
        ctx, "ColorPicker3: color must be an array of 3 numbers [r, g, b]");
  }

  int32_t flags = 0;
  if (argc >= 3) {
    if (JS_ToInt32(ctx, &flags, argv[2]) != 0) {
      JS_FreeCString(ctx, label);
      return JS_EXCEPTION;
    }
  }

  bool changed = ImGui::ColorPicker3(label, col, flags);
  JS_FreeCString(ctx, label);

  // Return object with {changed, color}
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "color", js_new_color_array(ctx, col, 3));

  return result;
}

// ImGui::ColorPicker4(const char* label, float col[4], ImGuiColorEditFlags
// flags = 0, const float* ref_col = NULL) JS: ColorPicker4(label, color,
// options?) - Returns {changed: bool, color: [r, g, b, a]} options = {flags:
// int, refCol: [r, g, b, a]} color: array [r, g, b, a] with values 0.0-1.0
// refCol: optional reference color array [r, g, b, a]
static JSValue js_ImGui_ColorPicker4(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "ColorPicker4: expected at least 2 arguments (label, color)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  float col[4];
  if (!js_get_color_array(ctx, argv[1], col, 4)) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(
        ctx, "ColorPicker4: color must be an array of 4 numbers [r, g, b, a]");
  }

  int32_t flags = 0;
  float ref_col[4];
  const float *ref_col_ptr = nullptr;

  // Parse optional options object
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      if (JS_ToInt32(ctx, &flags, flags_val) != 0) {
        JS_FreeValue(ctx, flags_val);
        JS_FreeCString(ctx, label);
        return JS_EXCEPTION;
      }
    }
    JS_FreeValue(ctx, flags_val);

    JSValue ref_col_val = JS_GetPropertyStr(ctx, argv[2], "refCol");
    if (!JS_IsUndefined(ref_col_val) && !JS_IsNull(ref_col_val)) {
      if (js_get_color_array(ctx, ref_col_val, ref_col, 4)) {
        ref_col_ptr = ref_col;
      }
    }
    JS_FreeValue(ctx, ref_col_val);
  }
  // Support flags as a direct third argument for backwards compatibility
  else if (argc >= 3) {
    if (JS_ToInt32(ctx, &flags, argv[2]) != 0) {
      JS_FreeCString(ctx, label);
      return JS_EXCEPTION;
    }

    // Check for optional fourth argument as ref_col
    if (argc >= 4 && JS_IsArray(argv[3])) {
      if (js_get_color_array(ctx, argv[3], ref_col, 4)) {
        ref_col_ptr = ref_col;
      }
    }
  }

  bool changed = ImGui::ColorPicker4(label, col, flags, ref_col_ptr);
  JS_FreeCString(ctx, label);

  // Return object with {changed, color}
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "color", js_new_color_array(ctx, col, 4));

  return result;
}

// ============================================================================
// Color Button Widget
// ============================================================================

// ImGui::ColorButton(const char* desc_id, const ImVec4& col,
// ImGuiColorEditFlags flags = 0, const ImVec2& size = ImVec2(0,0)) JS:
// ColorButton(desc_id, color, options?) - Returns bool color: array [r, g, b,
// a] or object {x, y, z, w} with values 0.0-1.0 options = {flags: int, size:
// {x, y}}
static JSValue js_ImGui_ColorButton(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "ColorButton: expected at least 2 arguments (desc_id, color)");
  }

  const char *desc_id = JS_ToCString(ctx, argv[0]);
  if (!desc_id) {
    return JS_EXCEPTION;
  }

  // Parse color - can be array [r, g, b, a] or object {x, y, z, w}
  ImVec4 col(0, 0, 0, 1);

  if (JS_IsArray(argv[1])) {
    float color_arr[4] = {0, 0, 0, 1};
    if (!js_get_color_array(ctx, argv[1], color_arr, 4)) {
      JS_FreeCString(ctx, desc_id);
      return JS_ThrowTypeError(
          ctx, "ColorButton: color must be an array of 4 numbers [r, g, b, a]");
    }
    col = ImVec4(color_arr[0], color_arr[1], color_arr[2], color_arr[3]);
  } else if (JS_IsObject(argv[1])) {
    JSValue x_val = JS_GetPropertyStr(ctx, argv[1], "x");
    JSValue y_val = JS_GetPropertyStr(ctx, argv[1], "y");
    JSValue z_val = JS_GetPropertyStr(ctx, argv[1], "z");
    JSValue w_val = JS_GetPropertyStr(ctx, argv[1], "w");

    double x, y, z, w;
    if (JS_ToFloat64(ctx, &x, x_val) == 0 &&
        JS_ToFloat64(ctx, &y, y_val) == 0 &&
        JS_ToFloat64(ctx, &z, z_val) == 0 &&
        JS_ToFloat64(ctx, &w, w_val) == 0) {
      col = ImVec4(static_cast<float>(x), static_cast<float>(y),
                   static_cast<float>(z), static_cast<float>(w));
    }

    JS_FreeValue(ctx, x_val);
    JS_FreeValue(ctx, y_val);
    JS_FreeValue(ctx, z_val);
    JS_FreeValue(ctx, w_val);
  } else {
    JS_FreeCString(ctx, desc_id);
    return JS_ThrowTypeError(ctx, "ColorButton: color must be an array [r, g, "
                                  "b, a] or object {x, y, z, w}");
  }

  int32_t flags = 0;
  ImVec2 size(0, 0);

  // Parse optional options object
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      if (JS_ToInt32(ctx, &flags, flags_val) != 0) {
        JS_FreeValue(ctx, flags_val);
        JS_FreeCString(ctx, desc_id);
        return JS_EXCEPTION;
      }
    }
    JS_FreeValue(ctx, flags_val);

    JSValue size_val = JS_GetPropertyStr(ctx, argv[2], "size");
    if (!JS_IsUndefined(size_val) && JS_IsObject(size_val)) {
      JSValue x_val = JS_GetPropertyStr(ctx, size_val, "x");
      JSValue y_val = JS_GetPropertyStr(ctx, size_val, "y");

      double x, y;
      if (JS_ToFloat64(ctx, &x, x_val) == 0 &&
          JS_ToFloat64(ctx, &y, y_val) == 0) {
        size.x = static_cast<float>(x);
        size.y = static_cast<float>(y);
      }

      JS_FreeValue(ctx, x_val);
      JS_FreeValue(ctx, y_val);
    }
    JS_FreeValue(ctx, size_val);
  }

  bool result = ImGui::ColorButton(desc_id, col, flags, size);
  JS_FreeCString(ctx, desc_id);

  return JS_NewBool(ctx, result);
}

// ============================================================================
// Color Utilities
// ============================================================================

// ImGui::SetColorEditOptions(ImGuiColorEditFlags flags)
// JS: SetColorEditOptions(flags) - void
static JSValue js_ImGui_SetColorEditOptions(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "SetColorEditOptions: expected 1 argument (flags)");
  }

  int32_t flags;
  if (JS_ToInt32(ctx, &flags, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  ImGui::SetColorEditOptions(flags);
  return JS_UNDEFINED;
}

// ============================================================================
// Layout Functions
// ============================================================================

// ImGui::Separator() - void
// Separator, generally horizontal. Can be vertical if in a menu bar or
// horizontal layout.
static JSValue js_ImGui_Separator(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  ImGui::Separator();
  return JS_UNDEFINED;
}

// ImGui::SameLine(float offset_from_start_x=0.0f, float spacing=-1.0f) - void
// Call between widgets to layout them horizontally.
static JSValue js_ImGui_SameLine(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  float offset_from_start_x = 0.0f;
  float spacing = -1.0f;

  // Extract optional parameters
  if (argc >= 1) {
    double temp;
    if (JS_ToFloat64(ctx, &temp, argv[0]) != 0) {
      return JS_ThrowTypeError(ctx, "offset_from_start_x must be a number");
    }
    offset_from_start_x = static_cast<float>(temp);
  }

  if (argc >= 2) {
    double temp;
    if (JS_ToFloat64(ctx, &temp, argv[1]) != 0) {
      return JS_ThrowTypeError(ctx, "spacing must be a number");
    }
    spacing = static_cast<float>(temp);
  }

  ImGui::SameLine(offset_from_start_x, spacing);
  return JS_UNDEFINED;
}

// ImGui::NewLine() - void
// Undo a SameLine() or force a new line when in a horizontal layout context.
static JSValue js_ImGui_NewLine(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  ImGui::NewLine();
  return JS_UNDEFINED;
}

// ImGui::Spacing() - void
// Add vertical spacing.
static JSValue js_ImGui_Spacing(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  ImGui::Spacing();
  return JS_UNDEFINED;
}

// ImGui::Indent(float indent_w = 0.0f) - void
// Move content position toward the right, by indent_w, or style.IndentSpacing
// if indent_w <= 0
static JSValue js_ImGui_Indent(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
  float indent_w = 0.0f;

  if (argc >= 1) {
    double temp;
    if (JS_ToFloat64(ctx, &temp, argv[0]) != 0) {
      return JS_ThrowTypeError(ctx, "indent_w must be a number");
    }
    indent_w = static_cast<float>(temp);
  }

  ImGui::Indent(indent_w);
  return JS_UNDEFINED;
}

// ImGui::Unindent(float indent_w = 0.0f) - void
// Move content position back to the left, by indent_w, or style.IndentSpacing
// if indent_w <= 0
static JSValue js_ImGui_Unindent(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  float indent_w = 0.0f;

  if (argc >= 1) {
    double temp;
    if (JS_ToFloat64(ctx, &temp, argv[0]) != 0) {
      return JS_ThrowTypeError(ctx, "indent_w must be a number");
    }
    indent_w = static_cast<float>(temp);
  }

  ImGui::Unindent(indent_w);
  return JS_UNDEFINED;
}

// ============================================================================
// Cursor Positioning Functions
// ============================================================================

// ImGui::GetCursorScreenPos() - returns ImVec2
// Cursor position in absolute screen coordinates (useful to work with
// ImDrawList API).
static JSValue js_ImGui_GetCursorScreenPos(JSContext *ctx,
                                           JSValueConst this_val, int argc,
                                           JSValueConst *argv) {
  ImVec2 pos = ImGui::GetCursorScreenPos();
  return ImVec2_to_js(ctx, pos);
}

// ImGui::SetCursorScreenPos(const ImVec2& pos) - void
// Set cursor position in absolute screen coordinates.
static JSValue js_ImGui_SetCursorScreenPos(JSContext *ctx,
                                           JSValueConst this_val, int argc,
                                           JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "SetCursorScreenPos requires 1 argument");
  }

  ImVec2 pos;
  if (!js_to_ImVec2(ctx, argv[0], pos)) {
    return JS_ThrowTypeError(ctx,
                             "pos must be an object with x and y properties");
  }

  ImGui::SetCursorScreenPos(pos);
  return JS_UNDEFINED;
}

// ImGui::GetContentRegionAvail() - returns ImVec2
// Available space from current cursor position to the end of the region.
static JSValue js_ImGui_GetContentRegionAvail(JSContext *ctx,
                                              JSValueConst this_val, int argc,
                                              JSValueConst *argv) {
  ImVec2 region = ImGui::GetContentRegionAvail();
  return ImVec2_to_js(ctx, region);
}

// ImGui::GetCursorPos() - returns ImVec2
// Cursor position in window coordinates (relative to window position).
static JSValue js_ImGui_GetCursorPos(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  ImVec2 pos = ImGui::GetCursorPos();
  return ImVec2_to_js(ctx, pos);
}

// ImGui::GetCursorPosX() - returns float
// Cursor X position in window coordinates.
static JSValue js_ImGui_GetCursorPosX(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  float x = ImGui::GetCursorPosX();
  return JS_NewFloat64(ctx, x);
}

// ImGui::GetCursorPosY() - returns float
// Cursor Y position in window coordinates.
static JSValue js_ImGui_GetCursorPosY(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  float y = ImGui::GetCursorPosY();
  return JS_NewFloat64(ctx, y);
}

// ImGui::SetCursorPos(const ImVec2& local_pos) - void
// Set cursor position in window coordinates.
static JSValue js_ImGui_SetCursorPos(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "SetCursorPos requires 1 argument");
  }

  ImVec2 pos;
  if (!js_to_ImVec2(ctx, argv[0], pos)) {
    return JS_ThrowTypeError(ctx,
                             "pos must be an object with x and y properties");
  }

  ImGui::SetCursorPos(pos);
  return JS_UNDEFINED;
}

// ImGui::SetCursorPosX(float local_x) - void
// Set cursor X position in window coordinates.
static JSValue js_ImGui_SetCursorPosX(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "SetCursorPosX requires 1 argument");
  }

  double x;
  if (JS_ToFloat64(ctx, &x, argv[0]) != 0) {
    return JS_ThrowTypeError(ctx, "local_x must be a number");
  }

  ImGui::SetCursorPosX(static_cast<float>(x));
  return JS_UNDEFINED;
}

// ImGui::SetCursorPosY(float local_y) - void
// Set cursor Y position in window coordinates.
static JSValue js_ImGui_SetCursorPosY(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "SetCursorPosY requires 1 argument");
  }

  double y;
  if (JS_ToFloat64(ctx, &y, argv[0]) != 0) {
    return JS_ThrowTypeError(ctx, "local_y must be a number");
  }

  ImGui::SetCursorPosY(static_cast<float>(y));
  return JS_UNDEFINED;
}

// ImGui::GetCursorStartPos() - returns ImVec2
// Initial cursor position in window coordinates.
static JSValue js_ImGui_GetCursorStartPos(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  ImVec2 pos = ImGui::GetCursorStartPos();
  return ImVec2_to_js(ctx, pos);
}

// ============================================================================
// Scrolling Functions
// ============================================================================

// ImGui::GetScrollX() - returns float
// Get scrolling amount on the horizontal axis [0..GetScrollMaxX()].
static JSValue js_ImGui_GetScrollX(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  float x = ImGui::GetScrollX();
  return JS_NewFloat64(ctx, x);
}

// ImGui::GetScrollY() - returns float
// Get scrolling amount on the vertical axis [0..GetScrollMaxY()].
static JSValue js_ImGui_GetScrollY(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  float y = ImGui::GetScrollY();
  return JS_NewFloat64(ctx, y);
}

// ImGui::SetScrollX(float scroll_x) - void
// Set scrolling amount on the horizontal axis [0..GetScrollMaxX()].
static JSValue js_ImGui_SetScrollX(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "SetScrollX requires 1 argument");
  }

  double x;
  if (JS_ToFloat64(ctx, &x, argv[0]) != 0) {
    return JS_ThrowTypeError(ctx, "scroll_x must be a number");
  }

  ImGui::SetScrollX(static_cast<float>(x));
  return JS_UNDEFINED;
}

// ImGui::SetScrollY(float scroll_y) - void
// Set scrolling amount on the vertical axis [0..GetScrollMaxY()].
static JSValue js_ImGui_SetScrollY(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "SetScrollY requires 1 argument");
  }

  double y;
  if (JS_ToFloat64(ctx, &y, argv[0]) != 0) {
    return JS_ThrowTypeError(ctx, "scroll_y must be a number");
  }

  ImGui::SetScrollY(static_cast<float>(y));
  return JS_UNDEFINED;
}

// ImGui::GetScrollMaxX() - returns float
// Get maximum scrolling amount on the horizontal axis ~~ ContentSize.x -
// WindowSize.x.
static JSValue js_ImGui_GetScrollMaxX(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  float max_x = ImGui::GetScrollMaxX();
  return JS_NewFloat64(ctx, max_x);
}

// ImGui::GetScrollMaxY() - returns float
// Get maximum scrolling amount on the vertical axis ~~ ContentSize.y -
// WindowSize.y.
static JSValue js_ImGui_GetScrollMaxY(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  float max_y = ImGui::GetScrollMaxY();
  return JS_NewFloat64(ctx, max_y);
}

// ImGui::SetScrollHereX(float center_x_ratio = 0.5f) - void
// Adjust scrolling amount to make current cursor position visible.
// center_x_ratio=0.0: left, 0.5: center, 1.0: right.
static JSValue js_ImGui_SetScrollHereX(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  float center_x_ratio = 0.5f;

  if (argc >= 1) {
    double temp;
    if (JS_ToFloat64(ctx, &temp, argv[0]) != 0) {
      return JS_ThrowTypeError(ctx, "center_x_ratio must be a number");
    }
    center_x_ratio = static_cast<float>(temp);
  }

  ImGui::SetScrollHereX(center_x_ratio);
  return JS_UNDEFINED;
}

// ImGui::SetScrollHereY(float center_y_ratio = 0.5f) - void
// Adjust scrolling amount to make current cursor position visible.
// center_y_ratio=0.0: top, 0.5: center, 1.0: bottom.
static JSValue js_ImGui_SetScrollHereY(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  float center_y_ratio = 0.5f;

  if (argc >= 1) {
    double temp;
    if (JS_ToFloat64(ctx, &temp, argv[0]) != 0) {
      return JS_ThrowTypeError(ctx, "center_y_ratio must be a number");
    }
    center_y_ratio = static_cast<float>(temp);
  }

  ImGui::SetScrollHereY(center_y_ratio);
  return JS_UNDEFINED;
}

// ImGui::BeginMenuBar() - returns bool
// JavaScript API: BeginMenuBar()
// Returns: bool
static JSValue js_ImGui_BeginMenuBar(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  bool result = ImGui::BeginMenuBar();
  return JS_NewBool(ctx, result);
}

// ImGui::EndMenuBar() - void
// JavaScript API: EndMenuBar()
static JSValue js_ImGui_EndMenuBar(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  ImGui::EndMenuBar();
  return JS_UNDEFINED;
}

// ImGui::BeginMainMenuBar() - returns bool
// JavaScript API: BeginMainMenuBar()
// Returns: bool
static JSValue js_ImGui_BeginMainMenuBar(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  bool result = ImGui::BeginMainMenuBar();
  return JS_NewBool(ctx, result);
}

// ImGui::EndMainMenuBar() - void
// JavaScript API: EndMainMenuBar()
static JSValue js_ImGui_EndMainMenuBar(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  ImGui::EndMainMenuBar();
  return JS_UNDEFINED;
}

// ImGui::BeginMenu(const char* label, bool enabled = true) - returns bool
// JavaScript API: BeginMenu(label, enabled?)
// enabled is optional, defaults to true
// Returns: bool
static JSValue js_ImGui_BeginMenu(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "BeginMenu requires at least 1 argument (label)");
  }

  // Get label
  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  // Get enabled (optional, default true)
  bool enabled = true;
  if (argc >= 2) {
    enabled = JS_ToBool(ctx, argv[1]);
  }

  bool result = ImGui::BeginMenu(label, enabled);
  JS_FreeCString(ctx, label);

  return JS_NewBool(ctx, result);
}

// ImGui::EndMenu() - void
// JavaScript API: EndMenu()
static JSValue js_ImGui_EndMenu(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  ImGui::EndMenu();
  return JS_UNDEFINED;
}

// ImGui::MenuItem(const char* label, const char* shortcut = NULL, bool selected
// = false, bool enabled = true) - returns bool Also handles: MenuItem(const
// char* label, const char* shortcut, bool* p_selected, bool enabled = true)
// JavaScript API: MenuItem(label, options?) where options = {shortcut: string,
// selected: bool, enabled: bool} Returns: bool or {clicked: bool, selected:
// bool} if selected was provided
static JSValue js_ImGui_MenuItem(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "MenuItem requires at least 1 argument (label)");
  }

  // Get label
  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  // Default values
  const char *shortcut = nullptr;
  bool selected = false;
  bool enabled = true;
  bool has_selected_output = false;

  // Parse options object if provided
  if (argc >= 2 && JS_IsObject(argv[1])) {
    // Check for shortcut
    JSValue shortcut_val = JS_GetPropertyStr(ctx, argv[1], "shortcut");
    if (JS_IsString(shortcut_val)) {
      shortcut = JS_ToCString(ctx, shortcut_val);
    }
    JS_FreeValue(ctx, shortcut_val);

    // Check for selected
    JSValue selected_val = JS_GetPropertyStr(ctx, argv[1], "selected");
    if (!JS_IsUndefined(selected_val)) {
      selected = JS_ToBool(ctx, selected_val);
      has_selected_output = true;
    }
    JS_FreeValue(ctx, selected_val);

    // Check for enabled
    JSValue enabled_val = JS_GetPropertyStr(ctx, argv[1], "enabled");
    if (!JS_IsUndefined(enabled_val)) {
      enabled = JS_ToBool(ctx, enabled_val);
    }
    JS_FreeValue(ctx, enabled_val);
  }

  // Call ImGui::MenuItem
  bool result;
  if (has_selected_output) {
    result = ImGui::MenuItem(label, shortcut, &selected, enabled);
  } else {
    result = ImGui::MenuItem(label, shortcut, selected, enabled);
  }

  // Cleanup
  JS_FreeCString(ctx, label);
  if (shortcut) {
    JS_FreeCString(ctx, shortcut);
  }

  // Return result
  if (has_selected_output) {
    // Return object with clicked and selected
    JSValue ret_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret_obj, "clicked", JS_NewBool(ctx, result));
    JS_SetPropertyStr(ctx, ret_obj, "selected", JS_NewBool(ctx, selected));
    return ret_obj;
  } else {
    return JS_NewBool(ctx, result);
  }
}

// ============================================================================
// Popup and Modal Functions
// ============================================================================

// ImGui::BeginPopup(const char* str_id, ImGuiWindowFlags flags = 0) - returns
// bool JavaScript API: BeginPopup(str_id, flags?) flags is optional, defaults
// to 0 Returns: bool
static JSValue js_ImGui_BeginPopup(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "BeginPopup requires at least 1 argument (str_id)");
  }

  // Get str_id
  const char *str_id = JS_ToCString(ctx, argv[0]);
  if (!str_id) {
    return JS_EXCEPTION;
  }

  // Get flags (optional, default 0)
  ImGuiWindowFlags flags = 0;
  if (argc >= 2) {
    int32_t flags_int;
    if (JS_ToInt32(ctx, &flags_int, argv[1]) == 0) {
      flags = static_cast<ImGuiWindowFlags>(flags_int);
    }
  }

  bool result = ImGui::BeginPopup(str_id, flags);
  JS_FreeCString(ctx, str_id);

  return JS_NewBool(ctx, result);
}

// ImGui::BeginPopupModal(const char* name, bool* p_open = NULL,
// ImGuiWindowFlags flags = 0) - returns bool JavaScript API:
// BeginPopupModal(name, options?) where options = {open: bool, flags: number}
// Returns: {result: bool, open: bool} or just bool if no open provided
static JSValue js_ImGui_BeginPopupModal(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "BeginPopupModal requires at least 1 argument (name)");
  }

  // Get name
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) {
    return JS_EXCEPTION;
  }

  // Default values
  bool has_open = false;
  bool open = true;
  ImGuiWindowFlags flags = 0;

  // Parse options object if provided
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue open_val = JS_GetPropertyStr(ctx, argv[1], "open");
    if (!JS_IsUndefined(open_val)) {
      has_open = true;
      open = JS_ToBool(ctx, open_val);
    }
    JS_FreeValue(ctx, open_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[1], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t flags_int;
      if (JS_ToInt32(ctx, &flags_int, flags_val) == 0) {
        flags = static_cast<ImGuiWindowFlags>(flags_int);
      }
    }
    JS_FreeValue(ctx, flags_val);
  }

  // Call ImGui::BeginPopupModal
  bool result = ImGui::BeginPopupModal(name, has_open ? &open : nullptr, flags);
  JS_FreeCString(ctx, name);

  // Return result
  if (has_open) {
    JSValue ret_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret_obj, "result", JS_NewBool(ctx, result));
    JS_SetPropertyStr(ctx, ret_obj, "open", JS_NewBool(ctx, open));
    return ret_obj;
  } else {
    return JS_NewBool(ctx, result);
  }
}

// ImGui::EndPopup() - void
// JavaScript API: EndPopup()
static JSValue js_ImGui_EndPopup(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  ImGui::EndPopup();
  return JS_UNDEFINED;
}

// ImGui::OpenPopup(const char* str_id, ImGuiPopupFlags popup_flags = 0) - void
// JavaScript API: OpenPopup(str_id, popup_flags?)
// popup_flags is optional, defaults to 0
static JSValue js_ImGui_OpenPopup(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "OpenPopup requires at least 1 argument (str_id)");
  }

  // Get str_id
  const char *str_id = JS_ToCString(ctx, argv[0]);
  if (!str_id) {
    return JS_EXCEPTION;
  }

  // Get popup_flags (optional, default 0)
  ImGuiPopupFlags popup_flags = 0;
  if (argc >= 2) {
    int32_t flags_int;
    if (JS_ToInt32(ctx, &flags_int, argv[1]) == 0) {
      popup_flags = static_cast<ImGuiPopupFlags>(flags_int);
    }
  }

  ImGui::OpenPopup(str_id, popup_flags);
  JS_FreeCString(ctx, str_id);

  return JS_UNDEFINED;
}

// ImGui::OpenPopupOnItemClick(const char* str_id = NULL, ImGuiPopupFlags
// popup_flags = 1) - void JavaScript API: OpenPopupOnItemClick(str_id?,
// popup_flags?) Both parameters are optional
static JSValue js_ImGui_OpenPopupOnItemClick(JSContext *ctx,
                                             JSValueConst this_val, int argc,
                                             JSValueConst *argv) {
  // Get str_id (optional, can be NULL)
  const char *str_id = nullptr;
  if (argc >= 1 && JS_IsString(argv[0])) {
    str_id = JS_ToCString(ctx, argv[0]);
    if (!str_id) {
      return JS_EXCEPTION;
    }
  }

  // Get popup_flags (optional, default 1)
  ImGuiPopupFlags popup_flags = 1;
  if (argc >= 2) {
    int32_t flags_int;
    if (JS_ToInt32(ctx, &flags_int, argv[1]) == 0) {
      popup_flags = static_cast<ImGuiPopupFlags>(flags_int);
    }
  }

  ImGui::OpenPopupOnItemClick(str_id, popup_flags);

  if (str_id) {
    JS_FreeCString(ctx, str_id);
  }

  return JS_UNDEFINED;
}

// ImGui::CloseCurrentPopup() - void
// JavaScript API: CloseCurrentPopup()
static JSValue js_ImGui_CloseCurrentPopup(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  ImGui::CloseCurrentPopup();
  return JS_UNDEFINED;
}

// ImGui::BeginPopupContextItem(const char* str_id = NULL, ImGuiPopupFlags
// popup_flags = 1) - returns bool JavaScript API:
// BeginPopupContextItem(str_id?, popup_flags?) Both parameters are optional
// Returns: bool
static JSValue js_ImGui_BeginPopupContextItem(JSContext *ctx,
                                              JSValueConst this_val, int argc,
                                              JSValueConst *argv) {
  // Get str_id (optional, can be NULL)
  const char *str_id = nullptr;
  if (argc >= 1 && JS_IsString(argv[0])) {
    str_id = JS_ToCString(ctx, argv[0]);
    if (!str_id) {
      return JS_EXCEPTION;
    }
  }

  // Get popup_flags (optional, default 1)
  ImGuiPopupFlags popup_flags = 1;
  if (argc >= 2) {
    int32_t flags_int;
    if (JS_ToInt32(ctx, &flags_int, argv[1]) == 0) {
      popup_flags = static_cast<ImGuiPopupFlags>(flags_int);
    }
  }

  bool result = ImGui::BeginPopupContextItem(str_id, popup_flags);

  if (str_id) {
    JS_FreeCString(ctx, str_id);
  }

  return JS_NewBool(ctx, result);
}

// ImGui::BeginPopupContextWindow(const char* str_id = NULL, ImGuiPopupFlags
// popup_flags = 1) - returns bool JavaScript API:
// BeginPopupContextWindow(str_id?, popup_flags?) Both parameters are optional
// Returns: bool
static JSValue js_ImGui_BeginPopupContextWindow(JSContext *ctx,
                                                JSValueConst this_val, int argc,
                                                JSValueConst *argv) {
  // Get str_id (optional, can be NULL)
  const char *str_id = nullptr;
  if (argc >= 1 && JS_IsString(argv[0])) {
    str_id = JS_ToCString(ctx, argv[0]);
    if (!str_id) {
      return JS_EXCEPTION;
    }
  }

  // Get popup_flags (optional, default 1)
  ImGuiPopupFlags popup_flags = 1;
  if (argc >= 2) {
    int32_t flags_int;
    if (JS_ToInt32(ctx, &flags_int, argv[1]) == 0) {
      popup_flags = static_cast<ImGuiPopupFlags>(flags_int);
    }
  }

  bool result = ImGui::BeginPopupContextWindow(str_id, popup_flags);

  if (str_id) {
    JS_FreeCString(ctx, str_id);
  }

  return JS_NewBool(ctx, result);
}

// ImGui::BeginPopupContextVoid(const char* str_id = NULL, ImGuiPopupFlags
// popup_flags = 1) - returns bool JavaScript API:
// BeginPopupContextVoid(str_id?, popup_flags?) Both parameters are optional
// Returns: bool
static JSValue js_ImGui_BeginPopupContextVoid(JSContext *ctx,
                                              JSValueConst this_val, int argc,
                                              JSValueConst *argv) {
  // Get str_id (optional, can be NULL)
  const char *str_id = nullptr;
  if (argc >= 1 && JS_IsString(argv[0])) {
    str_id = JS_ToCString(ctx, argv[0]);
    if (!str_id) {
      return JS_EXCEPTION;
    }
  }

  // Get popup_flags (optional, default 1)
  ImGuiPopupFlags popup_flags = 1;
  if (argc >= 2) {
    int32_t flags_int;
    if (JS_ToInt32(ctx, &flags_int, argv[1]) == 0) {
      popup_flags = static_cast<ImGuiPopupFlags>(flags_int);
    }
  }

  bool result = ImGui::BeginPopupContextVoid(str_id, popup_flags);

  if (str_id) {
    JS_FreeCString(ctx, str_id);
  }

  return JS_NewBool(ctx, result);
}

// ImGui::IsPopupOpen(const char* str_id, ImGuiPopupFlags flags = 0) - returns
// bool JavaScript API: IsPopupOpen(str_id, flags?) flags is optional, defaults
// to 0 Returns: bool
static JSValue js_ImGui_IsPopupOpen(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "IsPopupOpen requires at least 1 argument (str_id)");
  }

  // Get str_id
  const char *str_id = JS_ToCString(ctx, argv[0]);
  if (!str_id) {
    return JS_EXCEPTION;
  }

  // Get flags (optional, default 0)
  ImGuiPopupFlags flags = 0;
  if (argc >= 2) {
    int32_t flags_int;
    if (JS_ToInt32(ctx, &flags_int, argv[1]) == 0) {
      flags = static_cast<ImGuiPopupFlags>(flags_int);
    }
  }

  bool result = ImGui::IsPopupOpen(str_id, flags);
  JS_FreeCString(ctx, str_id);

  return JS_NewBool(ctx, result);
}

// ============================================================================
// Tooltip Functions
// ============================================================================

// ImGui::BeginTooltip() - returns bool
// JavaScript API: BeginTooltip()
// Returns: bool
static JSValue js_ImGui_BeginTooltip(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  bool result = ImGui::BeginTooltip();
  return JS_NewBool(ctx, result);
}

// ImGui::EndTooltip() - void
// JavaScript API: EndTooltip()
static JSValue js_ImGui_EndTooltip(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  ImGui::EndTooltip();
  return JS_UNDEFINED;
}

// ImGui::SetTooltip(const char* fmt, ...) - void
// JavaScript API: SetTooltip(text)
// Simplified to accept a single text string
static JSValue js_ImGui_SetTooltip(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "SetTooltip requires 1 argument (text)");
  }

  // Get text
  const char *text = JS_ToCString(ctx, argv[0]);
  if (!text) {
    return JS_EXCEPTION;
  }

  // Call ImGui::SetTooltip with %s to avoid format string issues
  ImGui::SetTooltip("%s", text);
  JS_FreeCString(ctx, text);

  return JS_UNDEFINED;
}

// ImGui::BeginItemTooltip() - returns bool
// JavaScript API: BeginItemTooltip()
// Returns: bool
static JSValue js_ImGui_BeginItemTooltip(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  bool result = ImGui::BeginItemTooltip();
  return JS_NewBool(ctx, result);
}

// ImGui::SetItemTooltip(const char* fmt, ...) - void
// JavaScript API: SetItemTooltip(text)
// Simplified to accept a single text string
static JSValue js_ImGui_SetItemTooltip(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "SetItemTooltip requires 1 argument (text)");
  }

  // Get text
  const char *text = JS_ToCString(ctx, argv[0]);
  if (!text) {
    return JS_EXCEPTION;
  }

  // Call ImGui::SetItemTooltip with %s to avoid format string issues
  ImGui::SetItemTooltip("%s", text);
  JS_FreeCString(ctx, text);

  return JS_UNDEFINED;
}

// ============================================================================
// Window Functions
// ============================================================================

// ImGui::Begin(const char* name, bool* p_open = NULL, ImGuiWindowFlags flags =
// 0) JavaScript API: Begin(name, options?) where options = {open: bool, flags:
// number} Returns: {result: bool, open: bool} or just bool if no p_open
static JSValue js_ImGui_Begin(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "Begin requires at least 1 argument (name)");
  }

  // Get window name
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) {
    return JS_EXCEPTION;
  }

  // Default values
  bool has_open = false;
  bool open = true;
  ImGuiWindowFlags flags = 0;

  // Parse options object if provided
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue open_val = JS_GetPropertyStr(ctx, argv[1], "open");
    if (!JS_IsUndefined(open_val)) {
      has_open = true;
      open = JS_ToBool(ctx, open_val);
    }
    JS_FreeValue(ctx, open_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[1], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t flags_int;
      if (JS_ToInt32(ctx, &flags_int, flags_val) == 0) {
        flags = static_cast<ImGuiWindowFlags>(flags_int);
      }
    }
    JS_FreeValue(ctx, flags_val);
  }

  // Call ImGui::Begin
  bool result = ImGui::Begin(name, has_open ? &open : nullptr, flags);
  JS_FreeCString(ctx, name);

  // Return result
  if (has_open) {
    JSValue ret_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret_obj, "result", JS_NewBool(ctx, result));
    JS_SetPropertyStr(ctx, ret_obj, "open", JS_NewBool(ctx, open));
    return ret_obj;
  } else {
    return JS_NewBool(ctx, result);
  }
}

// ImGui::End() - void
static JSValue js_ImGui_End(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  ImGui::End();
  return JS_UNDEFINED;
}

// ImGui::BeginChild - two overloads merged
// JavaScript API: BeginChild(id, options?) where id is string or number
// options = {size: {x, y}, childFlags: number, windowFlags: number}
// Returns: bool
static JSValue js_ImGui_BeginChild(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "BeginChild requires at least 1 argument (id)");
  }

  // Default values
  ImVec2 size(0, 0);
  ImGuiChildFlags child_flags = 0;
  ImGuiWindowFlags window_flags = 0;

  // Parse options object if provided
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue size_val = JS_GetPropertyStr(ctx, argv[1], "size");
    if (JS_IsObject(size_val)) {
      js_to_ImVec2(ctx, size_val, size);
    }
    JS_FreeValue(ctx, size_val);

    JSValue child_flags_val = JS_GetPropertyStr(ctx, argv[1], "childFlags");
    if (!JS_IsUndefined(child_flags_val)) {
      int32_t cf;
      if (JS_ToInt32(ctx, &cf, child_flags_val) == 0) {
        child_flags = static_cast<ImGuiChildFlags>(cf);
      }
    }
    JS_FreeValue(ctx, child_flags_val);

    JSValue window_flags_val = JS_GetPropertyStr(ctx, argv[1], "windowFlags");
    if (!JS_IsUndefined(window_flags_val)) {
      int32_t wf;
      if (JS_ToInt32(ctx, &wf, window_flags_val) == 0) {
        window_flags = static_cast<ImGuiWindowFlags>(wf);
      }
    }
    JS_FreeValue(ctx, window_flags_val);
  }

  bool result = false;

  // Check if first argument is string or number
  if (JS_IsString(argv[0])) {
    const char *str_id = JS_ToCString(ctx, argv[0]);
    if (!str_id) {
      return JS_EXCEPTION;
    }
    result = ImGui::BeginChild(str_id, size, child_flags, window_flags);
    JS_FreeCString(ctx, str_id);
  } else {
    uint32_t id;
    if (JS_ToUint32(ctx, &id, argv[0]) != 0) {
      return JS_ThrowTypeError(ctx, "BeginChild id must be string or number");
    }
    result = ImGui::BeginChild(static_cast<ImGuiID>(id), size, child_flags,
                               window_flags);
  }

  return JS_NewBool(ctx, result);
}

// ImGui::EndChild() - void
static JSValue js_ImGui_EndChild(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  ImGui::EndChild();
  return JS_UNDEFINED;
}

// ============================================================================
// Window Utilities
// ============================================================================

// ImGui::IsWindowAppearing() - returns bool
static JSValue js_ImGui_IsWindowAppearing(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  return JS_NewBool(ctx, ImGui::IsWindowAppearing());
}

// ImGui::IsWindowCollapsed() - returns bool
static JSValue js_ImGui_IsWindowCollapsed(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  return JS_NewBool(ctx, ImGui::IsWindowCollapsed());
}

// ImGui::IsWindowFocused(ImGuiFocusedFlags flags=0) - returns bool
static JSValue js_ImGui_IsWindowFocused(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  ImGuiFocusedFlags flags = 0;

  if (argc >= 1) {
    int32_t flags_int;
    if (JS_ToInt32(ctx, &flags_int, argv[0]) == 0) {
      flags = static_cast<ImGuiFocusedFlags>(flags_int);
    }
  }

  return JS_NewBool(ctx, ImGui::IsWindowFocused(flags));
}

// ImGui::IsWindowHovered(ImGuiHoveredFlags flags=0) - returns bool
static JSValue js_ImGui_IsWindowHovered(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  ImGuiHoveredFlags flags = 0;

  if (argc >= 1) {
    int32_t flags_int;
    if (JS_ToInt32(ctx, &flags_int, argv[0]) == 0) {
      flags = static_cast<ImGuiHoveredFlags>(flags_int);
    }
  }

  return JS_NewBool(ctx, ImGui::IsWindowHovered(flags));
}

// ImGui::GetWindowPos() - returns ImVec2
static JSValue js_ImGui_GetWindowPos(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  ImVec2 pos = ImGui::GetWindowPos();
  return ImVec2_to_js(ctx, pos);
}

// ImGui::GetWindowSize() - returns ImVec2
static JSValue js_ImGui_GetWindowSize(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  ImVec2 size = ImGui::GetWindowSize();
  return ImVec2_to_js(ctx, size);
}

// ImGui::GetWindowWidth() - returns float
static JSValue js_ImGui_GetWindowWidth(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  return JS_NewFloat64(ctx, ImGui::GetWindowWidth());
}

// ImGui::GetWindowHeight() - returns float
static JSValue js_ImGui_GetWindowHeight(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  return JS_NewFloat64(ctx, ImGui::GetWindowHeight());
}

// ============================================================================
// Window Manipulation Functions
// ============================================================================

// ImGui::SetNextWindowPos(const ImVec2& pos, ImGuiCond cond = 0, const ImVec2&
// pivot = ImVec2(0, 0)) JavaScript API: SetNextWindowPos(pos, options?) where
// pos = {x, y}, options = {cond: number, pivot: {x, y}}
static JSValue js_ImGui_SetNextWindowPos(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "SetNextWindowPos requires at least 1 argument (pos)");
  }

  // Get position
  ImVec2 pos(0, 0);
  if (!js_to_ImVec2(ctx, argv[0], pos)) {
    return JS_ThrowTypeError(ctx, "SetNextWindowPos pos must be {x, y} object");
  }

  // Default values
  ImGuiCond cond = 0;
  ImVec2 pivot(0, 0);

  // Parse options object if provided
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue cond_val = JS_GetPropertyStr(ctx, argv[1], "cond");
    if (!JS_IsUndefined(cond_val)) {
      int32_t cond_int;
      if (JS_ToInt32(ctx, &cond_int, cond_val) == 0) {
        cond = static_cast<ImGuiCond>(cond_int);
      }
    }
    JS_FreeValue(ctx, cond_val);

    JSValue pivot_val = JS_GetPropertyStr(ctx, argv[1], "pivot");
    if (JS_IsObject(pivot_val)) {
      js_to_ImVec2(ctx, pivot_val, pivot);
    }
    JS_FreeValue(ctx, pivot_val);
  }

  ImGui::SetNextWindowPos(pos, cond, pivot);
  return JS_UNDEFINED;
}

// ImGui::SetNextWindowSize(const ImVec2& size, ImGuiCond cond = 0)
// JavaScript API: SetNextWindowSize(size, options?) where size = {x, y},
// options = {cond: number}
static JSValue js_ImGui_SetNextWindowSize(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "SetNextWindowSize requires at least 1 argument (size)");
  }

  // Get size
  ImVec2 size(0, 0);
  if (!js_to_ImVec2(ctx, argv[0], size)) {
    return JS_ThrowTypeError(ctx,
                             "SetNextWindowSize size must be {x, y} object");
  }

  // Default cond
  ImGuiCond cond = 0;

  // Parse options object if provided
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue cond_val = JS_GetPropertyStr(ctx, argv[1], "cond");
    if (!JS_IsUndefined(cond_val)) {
      int32_t cond_int;
      if (JS_ToInt32(ctx, &cond_int, cond_val) == 0) {
        cond = static_cast<ImGuiCond>(cond_int);
      }
    }
    JS_FreeValue(ctx, cond_val);
  }

  ImGui::SetNextWindowSize(size, cond);
  return JS_UNDEFINED;
}

// ImGui::SetNextWindowCollapsed(bool collapsed, ImGuiCond cond = 0)
// JavaScript API: SetNextWindowCollapsed(collapsed, options?) where options =
// {cond: number}
static JSValue js_ImGui_SetNextWindowCollapsed(JSContext *ctx,
                                               JSValueConst this_val, int argc,
                                               JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "SetNextWindowCollapsed requires at least 1 argument (collapsed)");
  }

  // Get collapsed
  bool collapsed = JS_ToBool(ctx, argv[0]);

  // Default cond
  ImGuiCond cond = 0;

  // Parse options object if provided
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue cond_val = JS_GetPropertyStr(ctx, argv[1], "cond");
    if (!JS_IsUndefined(cond_val)) {
      int32_t cond_int;
      if (JS_ToInt32(ctx, &cond_int, cond_val) == 0) {
        cond = static_cast<ImGuiCond>(cond_int);
      }
    }
    JS_FreeValue(ctx, cond_val);
  }

  ImGui::SetNextWindowCollapsed(collapsed, cond);
  return JS_UNDEFINED;
}

// ImGui::SetNextWindowFocus() - void
static JSValue js_ImGui_SetNextWindowFocus(JSContext *ctx,
                                           JSValueConst this_val, int argc,
                                           JSValueConst *argv) {
  ImGui::SetNextWindowFocus();
  return JS_UNDEFINED;
}

// ImGui::SetNextWindowBgAlpha(float alpha) - void
static JSValue js_ImGui_SetNextWindowBgAlpha(JSContext *ctx,
                                             JSValueConst this_val, int argc,
                                             JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "SetNextWindowBgAlpha requires 1 argument (alpha)");
  }

  double alpha;
  if (JS_ToFloat64(ctx, &alpha, argv[0]) != 0) {
    return JS_ThrowTypeError(ctx,
                             "SetNextWindowBgAlpha alpha must be a number");
  }

  ImGui::SetNextWindowBgAlpha(static_cast<float>(alpha));
  return JS_UNDEFINED;
}

// ============================================================================
// Style Functions
// ============================================================================

// ImGui::PushStyleColor(ImGuiCol idx, ImU32 col)
// ImGui::PushStyleColor(ImGuiCol idx, const ImVec4& col)
// JS: PushStyleColor(idx, col) where col can be number (ImU32) or {x,y,z,w}
// (ImVec4)
static JSValue js_ImGui_PushStyleColor(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx,
                             "PushStyleColor: expected 2 arguments (idx, col)");
  }

  int32_t idx;
  if (JS_ToInt32(ctx, &idx, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  // Check if col is a number (ImU32) or object (ImVec4)
  if (JS_IsObject(argv[1])) {
    // ImVec4 version
    ImVec4 col = js_to_ImVec4(ctx, argv[1]);
    ImGui::PushStyleColor(idx, col);
  } else {
    // ImU32 version
    uint32_t col_u32;
    if (JS_ToUint32(ctx, &col_u32, argv[1]) != 0) {
      return JS_EXCEPTION;
    }
    ImGui::PushStyleColor(idx, col_u32);
  }

  return JS_UNDEFINED;
}

// ImGui::PopStyleColor(int count = 1)
// JS: PopStyleColor(count?)
static JSValue js_ImGui_PopStyleColor(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  int32_t count = 1;
  if (argc >= 1) {
    if (JS_ToInt32(ctx, &count, argv[0]) != 0) {
      return JS_EXCEPTION;
    }
  }

  ImGui::PopStyleColor(count);
  return JS_UNDEFINED;
}

// ImGui::PushStyleVar(ImGuiStyleVar idx, float val)
// ImGui::PushStyleVar(ImGuiStyleVar idx, const ImVec2& val)
// JS: PushStyleVar(idx, val) where val can be number (float) or {x,y} (ImVec2)
static JSValue js_ImGui_PushStyleVar(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx,
                             "PushStyleVar: expected 2 arguments (idx, val)");
  }

  int32_t idx;
  if (JS_ToInt32(ctx, &idx, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  // Check if val is a number (float) or object (ImVec2)
  if (JS_IsObject(argv[1])) {
    // ImVec2 version
    ImVec2 val = js_to_ImVec2(ctx, argv[1]);
    ImGui::PushStyleVar(idx, val);
  } else {
    // float version
    double val_f;
    if (JS_ToFloat64(ctx, &val_f, argv[1]) != 0) {
      return JS_EXCEPTION;
    }
    ImGui::PushStyleVar(idx, static_cast<float>(val_f));
  }

  return JS_UNDEFINED;
}

// ImGui::PushStyleVarX(ImGuiStyleVar idx, float val_x)
// JS: PushStyleVarX(idx, val_x)
static JSValue js_ImGui_PushStyleVarX(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "PushStyleVarX: expected 2 arguments (idx, val_x)");
  }

  int32_t idx;
  if (JS_ToInt32(ctx, &idx, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  double val_x;
  if (JS_ToFloat64(ctx, &val_x, argv[1]) != 0) {
    return JS_EXCEPTION;
  }

  ImGui::PushStyleVarX(idx, static_cast<float>(val_x));
  return JS_UNDEFINED;
}

// ImGui::PushStyleVarY(ImGuiStyleVar idx, float val_y)
// JS: PushStyleVarY(idx, val_y)
static JSValue js_ImGui_PushStyleVarY(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "PushStyleVarY: expected 2 arguments (idx, val_y)");
  }

  int32_t idx;
  if (JS_ToInt32(ctx, &idx, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  double val_y;
  if (JS_ToFloat64(ctx, &val_y, argv[1]) != 0) {
    return JS_EXCEPTION;
  }

  ImGui::PushStyleVarY(idx, static_cast<float>(val_y));
  return JS_UNDEFINED;
}

// ImGui::PopStyleVar(int count = 1)
// JS: PopStyleVar(count?)
static JSValue js_ImGui_PopStyleVar(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  int32_t count = 1;
  if (argc >= 1) {
    if (JS_ToInt32(ctx, &count, argv[0]) != 0) {
      return JS_EXCEPTION;
    }
  }

  ImGui::PopStyleVar(count);
  return JS_UNDEFINED;
}

// ============================================================================
// Item Functions
// ============================================================================

// ImGui::PushItemFlag(ImGuiItemFlags option, bool enabled)
// JS: PushItemFlag(option, enabled)
static JSValue js_ImGui_PushItemFlag(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "PushItemFlag: expected 2 arguments (option, enabled)");
  }

  int32_t option;
  if (JS_ToInt32(ctx, &option, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  bool enabled = JS_ToBool(ctx, argv[1]);

  ImGui::PushItemFlag(option, enabled);
  return JS_UNDEFINED;
}

// ImGui::PopItemFlag()
// JS: PopItemFlag()
static JSValue js_ImGui_PopItemFlag(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  ImGui::PopItemFlag();
  return JS_UNDEFINED;
}

// ImGui::PushItemWidth(float item_width)
// JS: PushItemWidth(item_width)
static JSValue js_ImGui_PushItemWidth(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "PushItemWidth: expected 1 argument (item_width)");
  }

  double item_width;
  if (JS_ToFloat64(ctx, &item_width, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  ImGui::PushItemWidth(static_cast<float>(item_width));
  return JS_UNDEFINED;
}

// ImGui::PopItemWidth()
// JS: PopItemWidth()
static JSValue js_ImGui_PopItemWidth(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  ImGui::PopItemWidth();
  return JS_UNDEFINED;
}

// ImGui::SetNextItemWidth(float item_width)
// JS: SetNextItemWidth(item_width)
static JSValue js_ImGui_SetNextItemWidth(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "SetNextItemWidth: expected 1 argument (item_width)");
  }

  double item_width;
  if (JS_ToFloat64(ctx, &item_width, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  ImGui::SetNextItemWidth(static_cast<float>(item_width));
  return JS_UNDEFINED;
}

// ImGui::CalcItemWidth()
// JS: CalcItemWidth() - Returns float
static JSValue js_ImGui_CalcItemWidth(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  float width = ImGui::CalcItemWidth();
  return JS_NewFloat64(ctx, width);
}

// ImGui::PushTextWrapPos(float wrap_local_pos_x = 0.0f)
// JS: PushTextWrapPos(wrap_local_pos_x?)
static JSValue js_ImGui_PushTextWrapPos(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  float wrap_local_pos_x = 0.0f;
  if (argc >= 1) {
    double val;
    if (JS_ToFloat64(ctx, &val, argv[0]) != 0) {
      return JS_EXCEPTION;
    }
    wrap_local_pos_x = static_cast<float>(val);
  }

  ImGui::PushTextWrapPos(wrap_local_pos_x);
  return JS_UNDEFINED;
}

// ImGui::PopTextWrapPos()
// JS: PopTextWrapPos()
static JSValue js_ImGui_PopTextWrapPos(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  ImGui::PopTextWrapPos();
  return JS_UNDEFINED;
}

// ============================================================================
// Font Functions
// ============================================================================

// ImGui::PushFont(ImFont* font)
// JS: PushFont(font) - For now, accepts null/undefined to push default font
// Note: Full font support would require wrapping ImFont* objects
static JSValue js_ImGui_PushFont(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  // For now, we only support pushing NULL (default font)
  // Full implementation would require wrapping ImFont* as a JS object
  ImGui::PushFont(nullptr);
  return JS_UNDEFINED;
}

// ImGui::PopFont()
// JS: PopFont()
static JSValue js_ImGui_PopFont(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  ImGui::PopFont();
  return JS_UNDEFINED;
}

// ImGui::GetFont()
// JS: GetFont() - Returns null for now (would need ImFont* wrapper)
static JSValue js_ImGui_GetFont(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  // Full implementation would require wrapping ImFont* as a JS object
  return JS_NULL;
}

// ImGui::GetFontSize()
// JS: GetFontSize() - Returns float
static JSValue js_ImGui_GetFontSize(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  float size = ImGui::GetFontSize();
  return JS_NewFloat64(ctx, size);
}

// ImGui::GetFontTexUvWhitePixel()
// JS: GetFontTexUvWhitePixel() - Returns {x, y}
static JSValue js_ImGui_GetFontTexUvWhitePixel(JSContext *ctx,
                                               JSValueConst this_val, int argc,
                                               JSValueConst *argv) {
  ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  return ImVec2_to_js(ctx, uv);
}

// ============================================================================
// Color Functions
// ============================================================================

// ImGui::GetColorU32(ImGuiCol idx, float alpha_mul = 1.0f)
// ImGui::GetColorU32(const ImVec4& col)
// ImGui::GetColorU32(ImU32 col, float alpha_mul = 1.0f)
// JS: GetColorU32(col_or_idx, alpha_mul?) - Returns ImU32
// Intelligently handles all three overloads based on parameter types
static JSValue js_ImGui_GetColorU32(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "GetColorU32: expected at least 1 argument");
  }

  float alpha_mul = 1.0f;
  if (argc >= 2) {
    double val;
    if (JS_ToFloat64(ctx, &val, argv[1]) != 0) {
      return JS_EXCEPTION;
    }
    alpha_mul = static_cast<float>(val);
  }

  ImU32 result;

  if (JS_IsObject(argv[0])) {
    // ImVec4 version: GetColorU32(const ImVec4& col)
    ImVec4 col = js_to_ImVec4(ctx, argv[0]);
    result = ImGui::GetColorU32(col);
  } else {
    // Could be ImGuiCol index or ImU32 color
    int32_t val_int;
    if (JS_ToInt32(ctx, &val_int, argv[0]) != 0) {
      return JS_EXCEPTION;
    }

    // Heuristic: If value is in ImGuiCol range (0-55), treat as index
    // Otherwise treat as ImU32 color value
    if (val_int >= 0 && val_int < ImGuiCol_COUNT) {
      // ImGuiCol version: GetColorU32(ImGuiCol idx, float alpha_mul = 1.0f)
      result = ImGui::GetColorU32(val_int, alpha_mul);
    } else {
      // ImU32 version: GetColorU32(ImU32 col, float alpha_mul = 1.0f)
      uint32_t col_u32 = static_cast<uint32_t>(val_int);
      result = ImGui::GetColorU32(col_u32, alpha_mul);
    }
  }

  return JS_NewUint32(ctx, result);
}

// ImGui::GetStyleColorVec4(ImGuiCol idx)
// JS: GetStyleColorVec4(idx) - Returns {x, y, z, w}
static JSValue js_ImGui_GetStyleColorVec4(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "GetStyleColorVec4: expected 1 argument (idx)");
  }

  int32_t idx;
  if (JS_ToInt32(ctx, &idx, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  const ImVec4 &col = ImGui::GetStyleColorVec4(idx);
  return ImVec4_to_js(ctx, col);
}

// ============================================================================
// Table Creation Functions
// ============================================================================

// ImGui::BeginTable(const char* str_id, int column, ImGuiTableFlags flags = 0,
//                   const ImVec2& outer_size = ImVec2(0.0f, 0.0f), float
//                   inner_width = 0.0f)
// JavaScript API: BeginTable(str_id, column, options?)
// options = {flags: number, outerSize: {x, y}, innerWidth: number}
// Returns: bool
static JSValue js_ImGui_BeginTable(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "BeginTable requires at least 2 arguments (str_id, column)");
  }

  // Get str_id
  const char *str_id = JS_ToCString(ctx, argv[0]);
  if (!str_id) {
    return JS_EXCEPTION;
  }

  // Get column count
  int32_t column;
  if (JS_ToInt32(ctx, &column, argv[1]) != 0) {
    JS_FreeCString(ctx, str_id);
    return JS_ThrowTypeError(ctx, "BeginTable column must be a number");
  }

  // Default values
  ImGuiTableFlags flags = 0;
  ImVec2 outer_size(0.0f, 0.0f);
  float inner_width = 0.0f;

  // Parse options object if provided
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t flags_int;
      if (JS_ToInt32(ctx, &flags_int, flags_val) == 0) {
        flags = static_cast<ImGuiTableFlags>(flags_int);
      }
    }
    JS_FreeValue(ctx, flags_val);

    JSValue outer_size_val = JS_GetPropertyStr(ctx, argv[2], "outerSize");
    if (JS_IsObject(outer_size_val)) {
      js_to_ImVec2(ctx, outer_size_val, outer_size);
    }
    JS_FreeValue(ctx, outer_size_val);

    JSValue inner_width_val = JS_GetPropertyStr(ctx, argv[2], "innerWidth");
    if (!JS_IsUndefined(inner_width_val)) {
      double iw;
      if (JS_ToFloat64(ctx, &iw, inner_width_val) == 0) {
        inner_width = static_cast<float>(iw);
      }
    }
    JS_FreeValue(ctx, inner_width_val);
  }

  bool result =
      ImGui::BeginTable(str_id, column, flags, outer_size, inner_width);
  JS_FreeCString(ctx, str_id);

  return JS_NewBool(ctx, result);
}

// ImGui::EndTable() - void
static JSValue js_ImGui_EndTable(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  ImGui::EndTable();
  return JS_UNDEFINED;
}

// ImGui::TableNextRow(ImGuiTableRowFlags row_flags = 0, float min_row_height =
// 0.0f) JavaScript API: TableNextRow(options?) options = {rowFlags: number,
// minRowHeight: number}
static JSValue js_ImGui_TableNextRow(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  // Default values
  ImGuiTableRowFlags row_flags = 0;
  float min_row_height = 0.0f;

  // Parse options object if provided
  if (argc >= 1 && JS_IsObject(argv[0])) {
    JSValue row_flags_val = JS_GetPropertyStr(ctx, argv[0], "rowFlags");
    if (!JS_IsUndefined(row_flags_val)) {
      int32_t rf;
      if (JS_ToInt32(ctx, &rf, row_flags_val) == 0) {
        row_flags = static_cast<ImGuiTableRowFlags>(rf);
      }
    }
    JS_FreeValue(ctx, row_flags_val);

    JSValue min_row_height_val =
        JS_GetPropertyStr(ctx, argv[0], "minRowHeight");
    if (!JS_IsUndefined(min_row_height_val)) {
      double mrh;
      if (JS_ToFloat64(ctx, &mrh, min_row_height_val) == 0) {
        min_row_height = static_cast<float>(mrh);
      }
    }
    JS_FreeValue(ctx, min_row_height_val);
  }

  ImGui::TableNextRow(row_flags, min_row_height);
  return JS_UNDEFINED;
}

// ImGui::TableNextColumn() - returns bool
static JSValue js_ImGui_TableNextColumn(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  bool result = ImGui::TableNextColumn();
  return JS_NewBool(ctx, result);
}

// ImGui::TableSetColumnIndex(int column_n) - returns bool
static JSValue js_ImGui_TableSetColumnIndex(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "TableSetColumnIndex requires 1 argument (column_n)");
  }

  int32_t column_n;
  if (JS_ToInt32(ctx, &column_n, argv[0]) != 0) {
    return JS_ThrowTypeError(ctx,
                             "TableSetColumnIndex column_n must be a number");
  }

  bool result = ImGui::TableSetColumnIndex(column_n);
  return JS_NewBool(ctx, result);
}

// ============================================================================
// Table Setup Functions
// ============================================================================

// ImGui::TableSetupColumn(const char* label, ImGuiTableColumnFlags flags = 0,
//                         float init_width_or_weight = 0.0f, ImGuiID user_id =
//                         0)
// JavaScript API: TableSetupColumn(label, options?)
// options = {flags: number, initWidthOrWeight: number, userId: number}
static JSValue js_ImGui_TableSetupColumn(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "TableSetupColumn requires at least 1 argument (label)");
  }

  // Get label
  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  // Default values
  ImGuiTableColumnFlags flags = 0;
  float init_width_or_weight = 0.0f;
  ImGuiID user_id = 0;

  // Parse options object if provided
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue flags_val = JS_GetPropertyStr(ctx, argv[1], "flags");
    if (!JS_IsUndefined(flags_val)) {
      int32_t flags_int;
      if (JS_ToInt32(ctx, &flags_int, flags_val) == 0) {
        flags = static_cast<ImGuiTableColumnFlags>(flags_int);
      }
    }
    JS_FreeValue(ctx, flags_val);

    JSValue init_val = JS_GetPropertyStr(ctx, argv[1], "initWidthOrWeight");
    if (!JS_IsUndefined(init_val)) {
      double iwow;
      if (JS_ToFloat64(ctx, &iwow, init_val) == 0) {
        init_width_or_weight = static_cast<float>(iwow);
      }
    }
    JS_FreeValue(ctx, init_val);

    JSValue user_id_val = JS_GetPropertyStr(ctx, argv[1], "userId");
    if (!JS_IsUndefined(user_id_val)) {
      uint32_t uid;
      if (JS_ToUint32(ctx, &uid, user_id_val) == 0) {
        user_id = static_cast<ImGuiID>(uid);
      }
    }
    JS_FreeValue(ctx, user_id_val);
  }

  ImGui::TableSetupColumn(label, flags, init_width_or_weight, user_id);
  JS_FreeCString(ctx, label);

  return JS_UNDEFINED;
}

// ImGui::TableSetupScrollFreeze(int cols, int rows) - void
static JSValue js_ImGui_TableSetupScrollFreeze(JSContext *ctx,
                                               JSValueConst this_val, int argc,
                                               JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "TableSetupScrollFreeze requires 2 arguments (cols, rows)");
  }

  int32_t cols, rows;
  if (JS_ToInt32(ctx, &cols, argv[0]) != 0 ||
      JS_ToInt32(ctx, &rows, argv[1]) != 0) {
    return JS_ThrowTypeError(
        ctx, "TableSetupScrollFreeze cols and rows must be numbers");
  }

  ImGui::TableSetupScrollFreeze(cols, rows);
  return JS_UNDEFINED;
}

// ImGui::TableHeader(const char* label) - void
static JSValue js_ImGui_TableHeader(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "TableHeader requires 1 argument (label)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  ImGui::TableHeader(label);
  JS_FreeCString(ctx, label);

  return JS_UNDEFINED;
}

// ImGui::TableHeadersRow() - void
static JSValue js_ImGui_TableHeadersRow(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  ImGui::TableHeadersRow();
  return JS_UNDEFINED;
}

// ImGui::TableAngledHeadersRow() - void
static JSValue js_ImGui_TableAngledHeadersRow(JSContext *ctx,
                                              JSValueConst this_val, int argc,
                                              JSValueConst *argv) {
  ImGui::TableAngledHeadersRow();
  return JS_UNDEFINED;
}

// ============================================================================
// Table Query Functions
// ============================================================================

// ImGui::TableGetColumnCount() - returns int
static JSValue js_ImGui_TableGetColumnCount(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  int result = ImGui::TableGetColumnCount();
  return JS_NewInt32(ctx, result);
}

// ImGui::TableGetColumnIndex() - returns int
static JSValue js_ImGui_TableGetColumnIndex(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  int result = ImGui::TableGetColumnIndex();
  return JS_NewInt32(ctx, result);
}

// ImGui::TableGetRowIndex() - returns int
static JSValue js_ImGui_TableGetRowIndex(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  int result = ImGui::TableGetRowIndex();
  return JS_NewInt32(ctx, result);
}

// ImGui::TableGetColumnName(int column_n = -1) - returns const char*
static JSValue js_ImGui_TableGetColumnName(JSContext *ctx,
                                           JSValueConst this_val, int argc,
                                           JSValueConst *argv) {
  int32_t column_n = -1;

  if (argc >= 1) {
    if (JS_ToInt32(ctx, &column_n, argv[0]) != 0) {
      return JS_ThrowTypeError(ctx,
                               "TableGetColumnName column_n must be a number");
    }
  }

  const char *result = ImGui::TableGetColumnName(column_n);
  if (result) {
    return JS_NewString(ctx, result);
  } else {
    return JS_NULL;
  }
}

// ImGui::TableGetColumnFlags(int column_n = -1) - returns ImGuiTableColumnFlags
static JSValue js_ImGui_TableGetColumnFlags(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  int32_t column_n = -1;

  if (argc >= 1) {
    if (JS_ToInt32(ctx, &column_n, argv[0]) != 0) {
      return JS_ThrowTypeError(ctx,
                               "TableGetColumnFlags column_n must be a number");
    }
  }

  ImGuiTableColumnFlags result = ImGui::TableGetColumnFlags(column_n);
  return JS_NewInt32(ctx, static_cast<int32_t>(result));
}

// ImGui::TableSetBgColor(ImGuiTableBgTarget target, ImU32 color, int column_n =
// -1) JavaScript API: TableSetBgColor(target, color, options?) options =
// {columnN: number}
static JSValue js_ImGui_TableSetBgColor(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "TableSetBgColor requires at least 2 arguments (target, color)");
  }

  // Get target
  int32_t target_int;
  if (JS_ToInt32(ctx, &target_int, argv[0]) != 0) {
    return JS_ThrowTypeError(ctx, "TableSetBgColor target must be a number");
  }
  ImGuiTableBgTarget target = static_cast<ImGuiTableBgTarget>(target_int);

  // Get color
  uint32_t color;
  if (JS_ToUint32(ctx, &color, argv[1]) != 0) {
    return JS_ThrowTypeError(ctx, "TableSetBgColor color must be a number");
  }

  // Default column_n
  int32_t column_n = -1;

  // Parse options object if provided
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue column_n_val = JS_GetPropertyStr(ctx, argv[2], "columnN");
    if (!JS_IsUndefined(column_n_val)) {
      JS_ToInt32(ctx, &column_n, column_n_val);
    }
    JS_FreeValue(ctx, column_n_val);
  }

  ImGui::TableSetBgColor(target, color, column_n);
  return JS_UNDEFINED;
}

// ============================================================================
// Table Sorting Functions
// ============================================================================

// ImGui::TableGetSortSpecs() - returns ImGuiTableSortSpecs*
// JavaScript API: Returns simplified object or null
// {
//   specsCount: number,
//   specsDirty: bool,
//   specs: [
//     {columnIndex: number, columnUserId: number, sortOrder: number,
//     sortDirection: number}
//   ]
// }
static JSValue js_ImGui_TableGetSortSpecs(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  ImGuiTableSortSpecs *sort_specs = ImGui::TableGetSortSpecs();

  if (!sort_specs) {
    return JS_NULL;
  }

  // Create return object
  JSValue obj = JS_NewObject(ctx);

  JS_SetPropertyStr(ctx, obj, "specsCount",
                    JS_NewInt32(ctx, sort_specs->SpecsCount));
  JS_SetPropertyStr(ctx, obj, "specsDirty",
                    JS_NewBool(ctx, sort_specs->SpecsDirty));

  // Create specs array
  JSValue specs_array = JS_NewArray(ctx);

  for (int i = 0; i < sort_specs->SpecsCount; i++) {
    const ImGuiTableColumnSortSpecs &spec = sort_specs->Specs[i];

    JSValue spec_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, spec_obj, "columnIndex",
                      JS_NewInt32(ctx, spec.ColumnIndex));
    JS_SetPropertyStr(ctx, spec_obj, "columnUserId",
                      JS_NewUint32(ctx, spec.ColumnUserID));
    JS_SetPropertyStr(ctx, spec_obj, "sortOrder",
                      JS_NewInt32(ctx, spec.SortOrder));
    JS_SetPropertyStr(
        ctx, spec_obj, "sortDirection",
        JS_NewInt32(ctx, static_cast<int32_t>(spec.SortDirection)));

    JS_SetPropertyUint32(ctx, specs_array, i, spec_obj);
  }

  JS_SetPropertyStr(ctx, obj, "specs", specs_array);

  return obj;
}

// ============================================================================
// Item/Widget Query Functions
// ============================================================================

// ImGui::IsItemHovered(ImGuiHoveredFlags flags = 0)
// JS: IsItemHovered(flags?) - Returns bool
static JSValue js_ImGui_IsItemHovered(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  int32_t flags = 0;
  if (argc >= 1) {
    if (JS_ToInt32(ctx, &flags, argv[0]) != 0) {
      return JS_EXCEPTION;
    }
  }

  bool result = ImGui::IsItemHovered(flags);
  return JS_NewBool(ctx, result);
}

// ImGui::IsItemActive()
// JS: IsItemActive() - Returns bool
static JSValue js_ImGui_IsItemActive(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  bool result = ImGui::IsItemActive();
  return JS_NewBool(ctx, result);
}

// ImGui::IsItemFocused()
// JS: IsItemFocused() - Returns bool
static JSValue js_ImGui_IsItemFocused(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  bool result = ImGui::IsItemFocused();
  return JS_NewBool(ctx, result);
}

// ImGui::IsItemClicked(ImGuiMouseButton mouse_button = 0)
// JS: IsItemClicked(mouse_button?) - Returns bool
static JSValue js_ImGui_IsItemClicked(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  int32_t mouse_button = 0;
  if (argc >= 1) {
    if (JS_ToInt32(ctx, &mouse_button, argv[0]) != 0) {
      return JS_EXCEPTION;
    }
  }

  bool result = ImGui::IsItemClicked(mouse_button);
  return JS_NewBool(ctx, result);
}

// ImGui::IsItemVisible()
// JS: IsItemVisible() - Returns bool
static JSValue js_ImGui_IsItemVisible(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  bool result = ImGui::IsItemVisible();
  return JS_NewBool(ctx, result);
}

// ImGui::IsItemEdited()
// JS: IsItemEdited() - Returns bool
static JSValue js_ImGui_IsItemEdited(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  bool result = ImGui::IsItemEdited();
  return JS_NewBool(ctx, result);
}

// ImGui::IsItemActivated()
// JS: IsItemActivated() - Returns bool
static JSValue js_ImGui_IsItemActivated(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  bool result = ImGui::IsItemActivated();
  return JS_NewBool(ctx, result);
}

// ImGui::IsItemDeactivated()
// JS: IsItemDeactivated() - Returns bool
static JSValue js_ImGui_IsItemDeactivated(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  bool result = ImGui::IsItemDeactivated();
  return JS_NewBool(ctx, result);
}

// ImGui::IsItemDeactivatedAfterEdit()
// JS: IsItemDeactivatedAfterEdit() - Returns bool
static JSValue js_ImGui_IsItemDeactivatedAfterEdit(JSContext *ctx,
                                                   JSValueConst this_val,
                                                   int argc,
                                                   JSValueConst *argv) {
  bool result = ImGui::IsItemDeactivatedAfterEdit();
  return JS_NewBool(ctx, result);
}

// ImGui::IsItemToggledOpen()
// JS: IsItemToggledOpen() - Returns bool
static JSValue js_ImGui_IsItemToggledOpen(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  bool result = ImGui::IsItemToggledOpen();
  return JS_NewBool(ctx, result);
}

// ImGui::IsAnyItemHovered()
// JS: IsAnyItemHovered() - Returns bool
static JSValue js_ImGui_IsAnyItemHovered(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  bool result = ImGui::IsAnyItemHovered();
  return JS_NewBool(ctx, result);
}

// ImGui::IsAnyItemActive()
// JS: IsAnyItemActive() - Returns bool
static JSValue js_ImGui_IsAnyItemActive(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  bool result = ImGui::IsAnyItemActive();
  return JS_NewBool(ctx, result);
}

// ImGui::IsAnyItemFocused()
// JS: IsAnyItemFocused() - Returns bool
static JSValue js_ImGui_IsAnyItemFocused(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  bool result = ImGui::IsAnyItemFocused();
  return JS_NewBool(ctx, result);
}

// ImGui::GetItemRectMin()
// JS: GetItemRectMin() - Returns {x, y}
static JSValue js_ImGui_GetItemRectMin(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  ImVec2 result = ImGui::GetItemRectMin();
  return ImVec2_to_js(ctx, result);
}

// ImGui::GetItemRectMax()
// JS: GetItemRectMax() - Returns {x, y}
static JSValue js_ImGui_GetItemRectMax(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  ImVec2 result = ImGui::GetItemRectMax();
  return ImVec2_to_js(ctx, result);
}

// ImGui::GetItemRectSize()
// JS: GetItemRectSize() - Returns {x, y}
static JSValue js_ImGui_GetItemRectSize(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  ImVec2 result = ImGui::GetItemRectSize();
  return ImVec2_to_js(ctx, result);
}

// ============================================================================
// Keyboard/Mouse/Gamepad Query Functions
// ============================================================================

// ImGui::IsKeyDown(ImGuiKey key)
// JS: IsKeyDown(key) - Returns bool
static JSValue js_ImGui_IsKeyDown(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "IsKeyDown: expected 1 argument (key)");
  }

  int32_t key;
  if (JS_ToInt32(ctx, &key, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  bool result = ImGui::IsKeyDown(static_cast<ImGuiKey>(key));
  return JS_NewBool(ctx, result);
}

// ImGui::IsKeyPressed(ImGuiKey key, bool repeat = true)
// JS: IsKeyPressed(key, repeat?) - Returns bool
static JSValue js_ImGui_IsKeyPressed(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "IsKeyPressed: expected at least 1 argument (key)");
  }

  int32_t key;
  if (JS_ToInt32(ctx, &key, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  bool repeat = true;
  if (argc >= 2) {
    repeat = JS_ToBool(ctx, argv[1]);
  }

  bool result = ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), repeat);
  return JS_NewBool(ctx, result);
}

// ImGui::IsKeyReleased(ImGuiKey key)
// JS: IsKeyReleased(key) - Returns bool
static JSValue js_ImGui_IsKeyReleased(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "IsKeyReleased: expected 1 argument (key)");
  }

  int32_t key;
  if (JS_ToInt32(ctx, &key, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  bool result = ImGui::IsKeyReleased(static_cast<ImGuiKey>(key));
  return JS_NewBool(ctx, result);
}

// ImGui::IsMouseDown(ImGuiMouseButton button)
// JS: IsMouseDown(button) - Returns bool
static JSValue js_ImGui_IsMouseDown(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "IsMouseDown: expected 1 argument (button)");
  }

  int32_t button;
  if (JS_ToInt32(ctx, &button, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  bool result = ImGui::IsMouseDown(button);
  return JS_NewBool(ctx, result);
}

// ImGui::IsMouseClicked(ImGuiMouseButton button, bool repeat = false)
// JS: IsMouseClicked(button, repeat?) - Returns bool
static JSValue js_ImGui_IsMouseClicked(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "IsMouseClicked: expected at least 1 argument (button)");
  }

  int32_t button;
  if (JS_ToInt32(ctx, &button, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  bool repeat = false;
  if (argc >= 2) {
    repeat = JS_ToBool(ctx, argv[1]);
  }

  bool result = ImGui::IsMouseClicked(button, repeat);
  return JS_NewBool(ctx, result);
}

// ImGui::IsMouseReleased(ImGuiMouseButton button)
// JS: IsMouseReleased(button) - Returns bool
static JSValue js_ImGui_IsMouseReleased(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "IsMouseReleased: expected 1 argument (button)");
  }

  int32_t button;
  if (JS_ToInt32(ctx, &button, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  bool result = ImGui::IsMouseReleased(button);
  return JS_NewBool(ctx, result);
}

// ImGui::IsMouseDoubleClicked(ImGuiMouseButton button)
// JS: IsMouseDoubleClicked(button) - Returns bool
static JSValue js_ImGui_IsMouseDoubleClicked(JSContext *ctx,
                                             JSValueConst this_val, int argc,
                                             JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "IsMouseDoubleClicked: expected 1 argument (button)");
  }

  int32_t button;
  if (JS_ToInt32(ctx, &button, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  bool result = ImGui::IsMouseDoubleClicked(button);
  return JS_NewBool(ctx, result);
}

// ImGui::IsMouseDragging(ImGuiMouseButton button, float lock_threshold = -1.0f)
// JS: IsMouseDragging(button, lock_threshold?) - Returns bool
static JSValue js_ImGui_IsMouseDragging(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "IsMouseDragging: expected at least 1 argument (button)");
  }

  int32_t button;
  if (JS_ToInt32(ctx, &button, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  float lock_threshold = -1.0f;
  if (argc >= 2) {
    double threshold;
    if (JS_ToFloat64(ctx, &threshold, argv[1]) != 0) {
      return JS_EXCEPTION;
    }
    lock_threshold = static_cast<float>(threshold);
  }

  bool result = ImGui::IsMouseDragging(button, lock_threshold);
  return JS_NewBool(ctx, result);
}

// ImGui::GetMousePos()
// JS: GetMousePos() - Returns {x, y}
static JSValue js_ImGui_GetMousePos(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  ImVec2 result = ImGui::GetMousePos();
  return ImVec2_to_js(ctx, result);
}

// ImGui::GetMousePosOnOpeningCurrentPopup()
// JS: GetMousePosOnOpeningCurrentPopup() - Returns {x, y}
static JSValue js_ImGui_GetMousePosOnOpeningCurrentPopup(JSContext *ctx,
                                                         JSValueConst this_val,
                                                         int argc,
                                                         JSValueConst *argv) {
  ImVec2 result = ImGui::GetMousePosOnOpeningCurrentPopup();
  return ImVec2_to_js(ctx, result);
}

// ImGui::GetMouseDragDelta(ImGuiMouseButton button = 0, float lock_threshold =
// -1.0f) JS: GetMouseDragDelta(button?, lock_threshold?) - Returns {x, y}
static JSValue js_ImGui_GetMouseDragDelta(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  int32_t button = 0;
  if (argc >= 1) {
    if (JS_ToInt32(ctx, &button, argv[0]) != 0) {
      return JS_EXCEPTION;
    }
  }

  float lock_threshold = -1.0f;
  if (argc >= 2) {
    double threshold;
    if (JS_ToFloat64(ctx, &threshold, argv[1]) != 0) {
      return JS_EXCEPTION;
    }
    lock_threshold = static_cast<float>(threshold);
  }

  ImVec2 result = ImGui::GetMouseDragDelta(button, lock_threshold);
  return ImVec2_to_js(ctx, result);
}

// ImGui::ResetMouseDragDelta(ImGuiMouseButton button = 0)
// JS: ResetMouseDragDelta(button?) - void
static JSValue js_ImGui_ResetMouseDragDelta(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  int32_t button = 0;
  if (argc >= 1) {
    if (JS_ToInt32(ctx, &button, argv[0]) != 0) {
      return JS_EXCEPTION;
    }
  }

  ImGui::ResetMouseDragDelta(button);
  return JS_UNDEFINED;
}

// ============================================================================
// Clipboard Functions
// ============================================================================

// ImGui::GetClipboardText()
// JS: GetClipboardText() - Returns string
static JSValue js_ImGui_GetClipboardText(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  const char *text = ImGui::GetClipboardText();
  if (!text) {
    return JS_NewString(ctx, "");
  }
  return JS_NewString(ctx, text);
}

// ImGui::SetClipboardText(const char* text)
// JS: SetClipboardText(text) - void
static JSValue js_ImGui_SetClipboardText(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx,
                             "SetClipboardText: expected 1 argument (text)");
  }

  const char *text = JS_ToCString(ctx, argv[0]);
  if (!text) {
    return JS_EXCEPTION;
  }

  ImGui::SetClipboardText(text);
  JS_FreeCString(ctx, text);

  return JS_UNDEFINED;
}

// ============================================================================
// Miscellaneous Utility Functions
// ============================================================================

// ImGui::SetItemDefaultFocus()
// JS: SetItemDefaultFocus() - void
static JSValue js_ImGui_SetItemDefaultFocus(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  ImGui::SetItemDefaultFocus();
  return JS_UNDEFINED;
}

// ImGui::SetKeyboardFocusHere(int offset = 0)
// JS: SetKeyboardFocusHere(offset?) - void
static JSValue js_ImGui_SetKeyboardFocusHere(JSContext *ctx,
                                             JSValueConst this_val, int argc,
                                             JSValueConst *argv) {
  int32_t offset = 0;
  if (argc >= 1) {
    if (JS_ToInt32(ctx, &offset, argv[0]) != 0) {
      return JS_EXCEPTION;
    }
  }

  ImGui::SetKeyboardFocusHere(offset);
  return JS_UNDEFINED;
}

// ImGui::Shortcut(ImGuiKeyChord key_chord, ImGuiInputFlags flags = 0)
// JS: Shortcut(key_chord, flags?) - Returns bool
static JSValue js_ImGui_Shortcut(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "Shortcut: expected at least 1 argument (key_chord)");
  }

  int32_t key_chord;
  if (JS_ToInt32(ctx, &key_chord, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  int32_t flags = 0;
  if (argc >= 2) {
    if (JS_ToInt32(ctx, &flags, argv[1]) != 0) {
      return JS_EXCEPTION;
    }
  }

  bool result = ImGui::Shortcut(key_chord, flags);
  return JS_NewBool(ctx, result);
}

// ImGui::SetNextItemShortcut(ImGuiKeyChord key_chord, ImGuiInputFlags flags =
// 0) JS: SetNextItemShortcut(key_chord, flags?) - void
static JSValue js_ImGui_SetNextItemShortcut(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "SetNextItemShortcut: expected at least 1 argument (key_chord)");
  }

  int32_t key_chord;
  if (JS_ToInt32(ctx, &key_chord, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  int32_t flags = 0;
  if (argc >= 2) {
    if (JS_ToInt32(ctx, &flags, argv[1]) != 0) {
      return JS_EXCEPTION;
    }
  }

  ImGui::SetNextItemShortcut(key_chord, flags);
  return JS_UNDEFINED;
}

// ImGui::SetNextItemAllowOverlap()
// JS: SetNextItemAllowOverlap() - void
static JSValue js_ImGui_SetNextItemAllowOverlap(JSContext *ctx,
                                                JSValueConst this_val, int argc,
                                                JSValueConst *argv) {
  ImGui::SetNextItemAllowOverlap();
  return JS_UNDEFINED;
}

// ImGui::BeginDisabled(bool disabled = true)
// JS: BeginDisabled(disabled?) - void
static JSValue js_ImGui_BeginDisabled(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  bool disabled = true;
  if (argc >= 1) {
    disabled = JS_ToBool(ctx, argv[0]);
  }

  ImGui::BeginDisabled(disabled);
  return JS_UNDEFINED;
}

// ImGui::EndDisabled()
// JS: EndDisabled() - void
static JSValue js_ImGui_EndDisabled(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  ImGui::EndDisabled();
  return JS_UNDEFINED;
}

// ============================================================================
// Tree Node Widgets
// ============================================================================

// ImGui::TreeNode(const char* label)
// JS: TreeNode(label) - Returns bool
static JSValue js_ImGui_TreeNode(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "TreeNode: expected 1 argument (label)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  bool result = ImGui::TreeNode(label);
  JS_FreeCString(ctx, label);

  return JS_NewBool(ctx, result);
}

// ImGui::TreeNode(const char* str_id, const char* fmt, ...)
// JS: TreeNodeV(str_id, label) - Returns bool
static JSValue js_ImGui_TreeNodeV(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx,
                             "TreeNodeV: expected 2 arguments (str_id, label)");
  }

  const char *str_id = JS_ToCString(ctx, argv[0]);
  if (!str_id) {
    return JS_EXCEPTION;
  }

  const char *label = JS_ToCString(ctx, argv[1]);
  if (!label) {
    JS_FreeCString(ctx, str_id);
    return JS_EXCEPTION;
  }

  bool result = ImGui::TreeNode(str_id, "%s", label);

  JS_FreeCString(ctx, str_id);
  JS_FreeCString(ctx, label);

  return JS_NewBool(ctx, result);
}

// ImGui::TreeNodeEx(const char* label, ImGuiTreeNodeFlags flags = 0)
// JS: TreeNodeEx(label, flags?) - Returns bool
static JSValue js_ImGui_TreeNodeEx(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "TreeNodeEx: expected at least 1 argument (label)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  int32_t flags = 0;
  if (argc >= 2) {
    if (JS_ToInt32(ctx, &flags, argv[1]) != 0) {
      JS_FreeCString(ctx, label);
      return JS_EXCEPTION;
    }
  }

  bool result = ImGui::TreeNodeEx(label, flags);
  JS_FreeCString(ctx, label);

  return JS_NewBool(ctx, result);
}

// ImGui::TreeNodeEx(const char* str_id, ImGuiTreeNodeFlags flags, const char*
// fmt, ...) JS: TreeNodeEx2(str_id, flags, label) - Returns bool
static JSValue js_ImGui_TreeNodeEx2(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 3) {
    return JS_ThrowTypeError(
        ctx, "TreeNodeEx2: expected 3 arguments (str_id, flags, label)");
  }

  const char *str_id = JS_ToCString(ctx, argv[0]);
  if (!str_id) {
    return JS_EXCEPTION;
  }

  int32_t flags;
  if (JS_ToInt32(ctx, &flags, argv[1]) != 0) {
    JS_FreeCString(ctx, str_id);
    return JS_EXCEPTION;
  }

  const char *label = JS_ToCString(ctx, argv[2]);
  if (!label) {
    JS_FreeCString(ctx, str_id);
    return JS_EXCEPTION;
  }

  bool result = ImGui::TreeNodeEx(str_id, flags, "%s", label);

  JS_FreeCString(ctx, str_id);
  JS_FreeCString(ctx, label);

  return JS_NewBool(ctx, result);
}

// ImGui::TreePush(const char* str_id)
// JS: TreePush(str_id) - void
static JSValue js_ImGui_TreePush(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "TreePush: expected 1 argument (str_id)");
  }

  const char *str_id = JS_ToCString(ctx, argv[0]);
  if (!str_id) {
    return JS_EXCEPTION;
  }

  ImGui::TreePush(str_id);
  JS_FreeCString(ctx, str_id);

  return JS_UNDEFINED;
}

// ImGui::TreePush(const void* ptr_id)
// JS: TreePushPtr(id_num) - void
// Note: JavaScript doesn't have raw pointers, so we use a number as an ID
static JSValue js_ImGui_TreePushPtr(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "TreePushPtr: expected 1 argument (id_num)");
  }

  int32_t id_num;
  if (JS_ToInt32(ctx, &id_num, argv[0]) != 0) {
    return JS_EXCEPTION;
  }

  // Cast the number to a pointer for ID purposes
  ImGui::TreePush(reinterpret_cast<void *>(static_cast<intptr_t>(id_num)));

  return JS_UNDEFINED;
}

// ImGui::TreePop()
// JS: TreePop() - void
static JSValue js_ImGui_TreePop(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  ImGui::TreePop();
  return JS_UNDEFINED;
}

// ImGui::GetTreeNodeToLabelSpacing()
// JS: GetTreeNodeToLabelSpacing() - Returns float
static JSValue js_ImGui_GetTreeNodeToLabelSpacing(JSContext *ctx,
                                                  JSValueConst this_val,
                                                  int argc,
                                                  JSValueConst *argv) {
  float spacing = ImGui::GetTreeNodeToLabelSpacing();
  return JS_NewFloat64(ctx, spacing);
}

// ImGui::CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0)
// JS: CollapsingHeader(label, flags?) - Returns bool
static JSValue js_ImGui_CollapsingHeader(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "CollapsingHeader: expected at least 1 argument (label)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  int32_t flags = 0;
  if (argc >= 2) {
    if (JS_ToInt32(ctx, &flags, argv[1]) != 0) {
      JS_FreeCString(ctx, label);
      return JS_EXCEPTION;
    }
  }

  bool result = ImGui::CollapsingHeader(label, flags);
  JS_FreeCString(ctx, label);

  return JS_NewBool(ctx, result);
}

// ImGui::CollapsingHeader(const char* label, bool* p_visible,
// ImGuiTreeNodeFlags flags = 0) JS: CollapsingHeaderEx(label, visible, flags?)
// - Returns {opened: bool, visible: bool}
static JSValue js_ImGui_CollapsingHeaderEx(JSContext *ctx,
                                           JSValueConst this_val, int argc,
                                           JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx,
        "CollapsingHeaderEx: expected at least 2 arguments (label, visible)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  bool visible = JS_ToBool(ctx, argv[1]);

  int32_t flags = 0;
  if (argc >= 3) {
    if (JS_ToInt32(ctx, &flags, argv[2]) != 0) {
      JS_FreeCString(ctx, label);
      return JS_EXCEPTION;
    }
  }

  bool opened = ImGui::CollapsingHeader(label, &visible, flags);
  JS_FreeCString(ctx, label);

  // Return object with {opened, visible}
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "opened", JS_NewBool(ctx, opened));
  JS_SetPropertyStr(ctx, result, "visible", JS_NewBool(ctx, visible));

  return result;
}

// ImGui::SetNextItemOpen(bool is_open, ImGuiCond cond = 0)
// JS: SetNextItemOpen(is_open, cond?) - void
static JSValue js_ImGui_SetNextItemOpen(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "SetNextItemOpen: expected at least 1 argument (is_open)");
  }

  bool is_open = JS_ToBool(ctx, argv[0]);

  int32_t cond = 0;
  if (argc >= 2) {
    if (JS_ToInt32(ctx, &cond, argv[1]) != 0) {
      return JS_EXCEPTION;
    }
  }

  ImGui::SetNextItemOpen(is_open, cond);
  return JS_UNDEFINED;
}

// ============================================================================
// Selectable Widgets
// ============================================================================

// ImGui::Selectable(const char* label, bool selected = false,
// ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0,0)) JS:
// Selectable(label, options?) where options = {selected: bool, flags: int,
// size: {x, y}} Returns bool
static JSValue js_ImGui_Selectable(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "Selectable: expected at least 1 argument (label)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  bool selected = false;
  int32_t flags = 0;
  ImVec2 size(0, 0);

  // Parse optional options object
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue selected_val = JS_GetPropertyStr(ctx, argv[1], "selected");
    if (!JS_IsUndefined(selected_val)) {
      selected = JS_ToBool(ctx, selected_val);
    }
    JS_FreeValue(ctx, selected_val);

    JSValue flags_val = JS_GetPropertyStr(ctx, argv[1], "flags");
    if (!JS_IsUndefined(flags_val)) {
      if (JS_ToInt32(ctx, &flags, flags_val) != 0) {
        JS_FreeValue(ctx, flags_val);
        JS_FreeCString(ctx, label);
        return JS_EXCEPTION;
      }
    }
    JS_FreeValue(ctx, flags_val);

    JSValue size_val = JS_GetPropertyStr(ctx, argv[1], "size");
    if (!JS_IsUndefined(size_val) && JS_IsObject(size_val)) {
      JSValue x_val = JS_GetPropertyStr(ctx, size_val, "x");
      JSValue y_val = JS_GetPropertyStr(ctx, size_val, "y");

      double x, y;
      if (JS_ToFloat64(ctx, &x, x_val) == 0 &&
          JS_ToFloat64(ctx, &y, y_val) == 0) {
        size.x = static_cast<float>(x);
        size.y = static_cast<float>(y);
      }

      JS_FreeValue(ctx, x_val);
      JS_FreeValue(ctx, y_val);
    }
    JS_FreeValue(ctx, size_val);
  }

  bool result = ImGui::Selectable(label, selected, flags, size);
  JS_FreeCString(ctx, label);

  return JS_NewBool(ctx, result);
}

// ImGui::Selectable(const char* label, bool* p_selected, ImGuiSelectableFlags
// flags = 0, const ImVec2& size = ImVec2(0,0)) JS: SelectableEx(label,
// selected, options?) where options = {flags: int, size: {x, y}} Returns
// {clicked: bool, selected: bool}
static JSValue js_ImGui_SelectableEx(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx, "SelectableEx: expected at least 2 arguments (label, selected)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  bool selected = JS_ToBool(ctx, argv[1]);

  int32_t flags = 0;
  ImVec2 size(0, 0);

  // Parse optional options object
  if (argc >= 3 && JS_IsObject(argv[2])) {
    JSValue flags_val = JS_GetPropertyStr(ctx, argv[2], "flags");
    if (!JS_IsUndefined(flags_val)) {
      if (JS_ToInt32(ctx, &flags, flags_val) != 0) {
        JS_FreeValue(ctx, flags_val);
        JS_FreeCString(ctx, label);
        return JS_EXCEPTION;
      }
    }
    JS_FreeValue(ctx, flags_val);

    JSValue size_val = JS_GetPropertyStr(ctx, argv[2], "size");
    if (!JS_IsUndefined(size_val) && JS_IsObject(size_val)) {
      JSValue x_val = JS_GetPropertyStr(ctx, size_val, "x");
      JSValue y_val = JS_GetPropertyStr(ctx, size_val, "y");

      double x, y;
      if (JS_ToFloat64(ctx, &x, x_val) == 0 &&
          JS_ToFloat64(ctx, &y, y_val) == 0) {
        size.x = static_cast<float>(x);
        size.y = static_cast<float>(y);
      }

      JS_FreeValue(ctx, x_val);
      JS_FreeValue(ctx, y_val);
    }
    JS_FreeValue(ctx, size_val);
  }

  bool clicked = ImGui::Selectable(label, &selected, flags, size);
  JS_FreeCString(ctx, label);

  // Return object with {clicked, selected}
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "clicked", JS_NewBool(ctx, clicked));
  JS_SetPropertyStr(ctx, result, "selected", JS_NewBool(ctx, selected));

  return result;
}

// ============================================================================
// List Box Widgets
// ============================================================================

// ImGui::BeginListBox(const char* label, const ImVec2& size = ImVec2(0,0))
// JS: BeginListBox(label, options?) where options = {size: {x, y}}
// Returns bool
static JSValue js_ImGui_BeginListBox(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "BeginListBox: expected at least 1 argument (label)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  ImVec2 size(0, 0);

  // Parse optional options object
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue size_val = JS_GetPropertyStr(ctx, argv[1], "size");
    if (!JS_IsUndefined(size_val) && JS_IsObject(size_val)) {
      JSValue x_val = JS_GetPropertyStr(ctx, size_val, "x");
      JSValue y_val = JS_GetPropertyStr(ctx, size_val, "y");

      double x, y;
      if (JS_ToFloat64(ctx, &x, x_val) == 0 &&
          JS_ToFloat64(ctx, &y, y_val) == 0) {
        size.x = static_cast<float>(x);
        size.y = static_cast<float>(y);
      }

      JS_FreeValue(ctx, x_val);
      JS_FreeValue(ctx, y_val);
    }
    JS_FreeValue(ctx, size_val);
  }

  bool result = ImGui::BeginListBox(label, size);
  JS_FreeCString(ctx, label);

  return JS_NewBool(ctx, result);
}

// ImGui::EndListBox()
// JS: EndListBox() - void
static JSValue js_ImGui_EndListBox(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  ImGui::EndListBox();
  return JS_UNDEFINED;
}

// ImGui::ListBox(const char* label, int* current_item, const char* const
// items[], int items_count, int height_in_items = -1) JS: ListBox(label,
// current_item, items, height_in_items?) - Returns {changed: bool,
// current_item: int}
static JSValue js_ImGui_ListBox(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  if (argc < 3) {
    return JS_ThrowTypeError(
        ctx,
        "ListBox: expected at least 3 arguments (label, current_item, items)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  int32_t current_item;
  if (JS_ToInt32(ctx, &current_item, argv[1]) != 0) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  // Parse items array
  if (!JS_IsArray(argv[2])) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx, "ListBox: items must be an array");
  }

  JSValue length_val = JS_GetPropertyStr(ctx, argv[2], "length");
  int32_t items_count;
  if (JS_ToInt32(ctx, &items_count, length_val) != 0) {
    JS_FreeValue(ctx, length_val);
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }
  JS_FreeValue(ctx, length_val);

  // Extract strings from array
  std::vector<const char *> items_vec;
  std::vector<JSValue> js_strings;
  items_vec.reserve(items_count);
  js_strings.reserve(items_count);

  for (int32_t i = 0; i < items_count; i++) {
    JSValue item_val = JS_GetPropertyUint32(ctx, argv[2], i);
    const char *item_str = JS_ToCString(ctx, item_val);
    if (!item_str) {
      // Cleanup on error
      for (size_t j = 0; j < items_vec.size(); j++) {
        JS_FreeCString(ctx, items_vec[j]);
      }
      JS_FreeValue(ctx, item_val);
      JS_FreeCString(ctx, label);
      return JS_EXCEPTION;
    }
    items_vec.push_back(item_str);
    js_strings.push_back(item_val);
  }

  int32_t height_in_items = -1;
  if (argc >= 4) {
    if (JS_ToInt32(ctx, &height_in_items, argv[3]) != 0) {
      for (size_t i = 0; i < items_vec.size(); i++) {
        JS_FreeCString(ctx, items_vec[i]);
      }
      for (auto &val : js_strings) {
        JS_FreeValue(ctx, val);
      }
      JS_FreeCString(ctx, label);
      return JS_EXCEPTION;
    }
  }

  bool changed = ImGui::ListBox(label, &current_item, items_vec.data(),
                                items_count, height_in_items);

  // Cleanup
  for (size_t i = 0; i < items_vec.size(); i++) {
    JS_FreeCString(ctx, items_vec[i]);
  }
  for (auto &val : js_strings) {
    JS_FreeValue(ctx, val);
  }
  JS_FreeCString(ctx, label);

  // Return object with {changed, current_item}
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "current_item",
                    JS_NewInt32(ctx, current_item));

  return result;
}

// ============================================================================
// Combo Widgets
// ============================================================================

// ImGui::BeginCombo(const char* label, const char* preview_value,
// ImGuiComboFlags flags = 0) JS: BeginCombo(label, preview_value, flags?) -
// Returns bool
static JSValue js_ImGui_BeginCombo(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(
        ctx,
        "BeginCombo: expected at least 2 arguments (label, preview_value)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  const char *preview_value = JS_ToCString(ctx, argv[1]);
  if (!preview_value) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  int32_t flags = 0;
  if (argc >= 3) {
    if (JS_ToInt32(ctx, &flags, argv[2]) != 0) {
      JS_FreeCString(ctx, label);
      JS_FreeCString(ctx, preview_value);
      return JS_EXCEPTION;
    }
  }

  bool result = ImGui::BeginCombo(label, preview_value, flags);

  JS_FreeCString(ctx, label);
  JS_FreeCString(ctx, preview_value);

  return JS_NewBool(ctx, result);
}

// ImGui::EndCombo()
// JS: EndCombo() - void
static JSValue js_ImGui_EndCombo(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  ImGui::EndCombo();
  return JS_UNDEFINED;
}

// ImGui::Combo(const char* label, int* current_item, const char* const items[],
// int items_count, int popup_max_height_in_items = -1) JS: Combo(label,
// current_item, items, popup_max_height_in_items?) - Returns {changed: bool,
// current_item: int}
static JSValue js_ImGui_Combo(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  if (argc < 3) {
    return JS_ThrowTypeError(
        ctx,
        "Combo: expected at least 3 arguments (label, current_item, items)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  int32_t current_item;
  if (JS_ToInt32(ctx, &current_item, argv[1]) != 0) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  // Parse items array
  if (!JS_IsArray(argv[2])) {
    JS_FreeCString(ctx, label);
    return JS_ThrowTypeError(ctx, "Combo: items must be an array");
  }

  JSValue length_val = JS_GetPropertyStr(ctx, argv[2], "length");
  int32_t items_count;
  if (JS_ToInt32(ctx, &items_count, length_val) != 0) {
    JS_FreeValue(ctx, length_val);
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }
  JS_FreeValue(ctx, length_val);

  // Extract strings from array
  std::vector<const char *> items_vec;
  std::vector<JSValue> js_strings;
  items_vec.reserve(items_count);
  js_strings.reserve(items_count);

  for (int32_t i = 0; i < items_count; i++) {
    JSValue item_val = JS_GetPropertyUint32(ctx, argv[2], i);
    const char *item_str = JS_ToCString(ctx, item_val);
    if (!item_str) {
      // Cleanup on error
      for (size_t j = 0; j < items_vec.size(); j++) {
        JS_FreeCString(ctx, items_vec[j]);
      }
      JS_FreeValue(ctx, item_val);
      JS_FreeCString(ctx, label);
      return JS_EXCEPTION;
    }
    items_vec.push_back(item_str);
    js_strings.push_back(item_val);
  }

  int32_t popup_max_height_in_items = -1;
  if (argc >= 4) {
    if (JS_ToInt32(ctx, &popup_max_height_in_items, argv[3]) != 0) {
      for (size_t i = 0; i < items_vec.size(); i++) {
        JS_FreeCString(ctx, items_vec[i]);
      }
      for (auto &val : js_strings) {
        JS_FreeValue(ctx, val);
      }
      JS_FreeCString(ctx, label);
      return JS_EXCEPTION;
    }
  }

  bool changed = ImGui::Combo(label, &current_item, items_vec.data(),
                              items_count, popup_max_height_in_items);

  // Cleanup
  for (size_t i = 0; i < items_vec.size(); i++) {
    JS_FreeCString(ctx, items_vec[i]);
  }
  for (auto &val : js_strings) {
    JS_FreeValue(ctx, val);
  }
  JS_FreeCString(ctx, label);

  // Return object with {changed, current_item}
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "current_item",
                    JS_NewInt32(ctx, current_item));

  return result;
}

// ImGui::Combo(const char* label, int* current_item, const char*
// items_separated_by_zeros, int popup_max_height_in_items = -1) JS:
// ComboSimple(label, current_item, items_separated_by_zeros,
// popup_max_height_in_items?) - Returns {changed: bool, current_item: int}
static JSValue js_ImGui_ComboSimple(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  if (argc < 3) {
    return JS_ThrowTypeError(ctx,
                             "ComboSimple: expected at least 3 arguments "
                             "(label, current_item, items_separated_by_zeros)");
  }

  const char *label = JS_ToCString(ctx, argv[0]);
  if (!label) {
    return JS_EXCEPTION;
  }

  int32_t current_item;
  if (JS_ToInt32(ctx, &current_item, argv[1]) != 0) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  const char *items_separated_by_zeros = JS_ToCString(ctx, argv[2]);
  if (!items_separated_by_zeros) {
    JS_FreeCString(ctx, label);
    return JS_EXCEPTION;
  }

  int32_t popup_max_height_in_items = -1;
  if (argc >= 4) {
    if (JS_ToInt32(ctx, &popup_max_height_in_items, argv[3]) != 0) {
      JS_FreeCString(ctx, label);
      JS_FreeCString(ctx, items_separated_by_zeros);
      return JS_EXCEPTION;
    }
  }

  bool changed = ImGui::Combo(label, &current_item, items_separated_by_zeros,
                              popup_max_height_in_items);

  JS_FreeCString(ctx, label);
  JS_FreeCString(ctx, items_separated_by_zeros);

  // Return object with {changed, current_item}
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "changed", JS_NewBool(ctx, changed));
  JS_SetPropertyStr(ctx, result, "current_item",
                    JS_NewInt32(ctx, current_item));

  return result;
}

const JSCFunctionListEntry js_imgui_funcs[] = {
    // Enums
    JS_ENUM_DEF(WindowFlags),
    JS_ENUM_DEF(ChildFlags),
    JS_ENUM_DEF(ItemFlags),
    JS_ENUM_DEF(InputTextFlags),
    JS_ENUM_DEF(SliderFlags),
    JS_ENUM_DEF(TreeNodeFlags),
    JS_ENUM_DEF(PopupFlags),
    JS_ENUM_DEF(SelectableFlags),
    JS_ENUM_DEF(ComboFlags),
    JS_ENUM_DEF(TabBarFlags),
    JS_ENUM_DEF(TabItemFlags),
    JS_ENUM_DEF(FocusedFlags),
    JS_ENUM_DEF(HoveredFlags),
    JS_ENUM_DEF(DragDropFlags),
    JS_ENUM_DEF(Dir),
    JS_ENUM_DEF(SortDirection),
    JS_ENUM_DEF(Key),
    JS_ENUM_DEF(Mod),
    JS_ENUM_DEF(MouseButton),
    JS_ENUM_DEF(InputFlags),
    JS_ENUM_DEF(ConfigFlags),
    JS_ENUM_DEF(Col),
    JS_ENUM_DEF(ButtonFlags),
    JS_ENUM_DEF(ColorEditFlags),
    JS_ENUM_DEF(TableFlags),
    JS_ENUM_DEF(TableColumnFlags),
    JS_ENUM_DEF(TableRowFlags),
    JS_ENUM_DEF(TableBgTarget),

    // Text Widgets
    JS_CFUNC_DEF("Text", 1, js_ImGui_Text),
    JS_CFUNC_DEF("TextColored", 2, js_ImGui_TextColored),
    JS_CFUNC_DEF("TextDisabled", 1, js_ImGui_TextDisabled),
    JS_CFUNC_DEF("TextWrapped", 1, js_ImGui_TextWrapped),
    JS_CFUNC_DEF("LabelText", 2, js_ImGui_LabelText),
    JS_CFUNC_DEF("BulletText", 1, js_ImGui_BulletText),
    JS_CFUNC_DEF("SeparatorText", 1, js_ImGui_SeparatorText),

    // Button Widgets
    JS_CFUNC_DEF("Button", 1, js_ImGui_Button),
    JS_CFUNC_DEF("SmallButton", 1, js_ImGui_SmallButton),
    JS_CFUNC_DEF("InvisibleButton", 2, js_ImGui_InvisibleButton),
    JS_CFUNC_DEF("ArrowButton", 2, js_ImGui_ArrowButton),
    JS_CFUNC_DEF("Checkbox", 2, js_ImGui_Checkbox),
    JS_CFUNC_DEF("RadioButton", 2, js_ImGui_RadioButton),
    JS_CFUNC_DEF("RadioButtonEx", 3, js_ImGui_RadioButtonEx),

    // Other Basic Widgets
    JS_CFUNC_DEF("ProgressBar", 1, js_ImGui_ProgressBar),
    JS_CFUNC_DEF("Bullet", 0, js_ImGui_Bullet),
    JS_CFUNC_DEF("Image", 2, js_ImGui_Image),
    JS_CFUNC_DEF("ImageButton", 3, js_ImGui_ImageButton),

    // Plot Widgets
    JS_CFUNC_DEF("PlotLines", 2, js_ImGui_PlotLines),
    JS_CFUNC_DEF("PlotHistogram", 2, js_ImGui_PlotHistogram),

    // Input Text Widgets
    JS_CFUNC_DEF("InputText", 2, js_ImGui_InputText),
    JS_CFUNC_DEF("InputTextMultiline", 2, js_ImGui_InputTextMultiline),
    JS_CFUNC_DEF("InputTextWithHint", 3, js_ImGui_InputTextWithHint),

    // Input Number Widgets
    JS_CFUNC_DEF("InputFloat", 2, js_ImGui_InputFloat),
    JS_CFUNC_DEF("InputFloat2", 2, js_ImGui_InputFloat2),
    JS_CFUNC_DEF("InputFloat3", 2, js_ImGui_InputFloat3),
    JS_CFUNC_DEF("InputFloat4", 2, js_ImGui_InputFloat4),
    JS_CFUNC_DEF("InputInt", 2, js_ImGui_InputInt),
    JS_CFUNC_DEF("InputInt2", 2, js_ImGui_InputInt2),
    JS_CFUNC_DEF("InputInt3", 2, js_ImGui_InputInt3),
    JS_CFUNC_DEF("InputInt4", 2, js_ImGui_InputInt4),

    // Slider Widgets
    JS_CFUNC_DEF("SliderFloat", 4, js_ImGui_SliderFloat),
    JS_CFUNC_DEF("SliderFloat2", 4, js_ImGui_SliderFloat2),
    JS_CFUNC_DEF("SliderFloat3", 4, js_ImGui_SliderFloat3),
    JS_CFUNC_DEF("SliderFloat4", 4, js_ImGui_SliderFloat4),
    JS_CFUNC_DEF("SliderInt", 4, js_ImGui_SliderInt),
    JS_CFUNC_DEF("SliderAngle", 2, js_ImGui_SliderAngle),

    // Drag Widgets
    JS_CFUNC_DEF("DragFloat", 2, js_ImGui_DragFloat),
    JS_CFUNC_DEF("DragFloat2", 2, js_ImGui_DragFloat2),
    JS_CFUNC_DEF("DragInt", 2, js_ImGui_DragInt),

    // Color Editors and Pickers
    JS_CFUNC_DEF("ColorEdit3", 2, js_ImGui_ColorEdit3),
    JS_CFUNC_DEF("ColorEdit4", 2, js_ImGui_ColorEdit4),
    JS_CFUNC_DEF("ColorPicker3", 2, js_ImGui_ColorPicker3),
    JS_CFUNC_DEF("ColorPicker4", 2, js_ImGui_ColorPicker4),
    JS_CFUNC_DEF("ColorButton", 2, js_ImGui_ColorButton),
    JS_CFUNC_DEF("SetColorEditOptions", 1, js_ImGui_SetColorEditOptions),

    // Layout Functions
    JS_CFUNC_DEF("Separator", 0, js_ImGui_Separator),
    JS_CFUNC_DEF("SameLine", 0, js_ImGui_SameLine),
    JS_CFUNC_DEF("NewLine", 0, js_ImGui_NewLine),
    JS_CFUNC_DEF("Spacing", 0, js_ImGui_Spacing),
    JS_CFUNC_DEF("Indent", 0, js_ImGui_Indent),
    JS_CFUNC_DEF("Unindent", 0, js_ImGui_Unindent),

    // Cursor Positioning Functions
    JS_CFUNC_DEF("GetCursorScreenPos", 0, js_ImGui_GetCursorScreenPos),
    JS_CFUNC_DEF("SetCursorScreenPos", 1, js_ImGui_SetCursorScreenPos),
    JS_CFUNC_DEF("GetContentRegionAvail", 0, js_ImGui_GetContentRegionAvail),
    JS_CFUNC_DEF("GetCursorPos", 0, js_ImGui_GetCursorPos),
    JS_CFUNC_DEF("GetCursorPosX", 0, js_ImGui_GetCursorPosX),
    JS_CFUNC_DEF("GetCursorPosY", 0, js_ImGui_GetCursorPosY),
    JS_CFUNC_DEF("SetCursorPos", 1, js_ImGui_SetCursorPos),
    JS_CFUNC_DEF("SetCursorPosX", 1, js_ImGui_SetCursorPosX),
    JS_CFUNC_DEF("SetCursorPosY", 1, js_ImGui_SetCursorPosY),
    JS_CFUNC_DEF("GetCursorStartPos", 0, js_ImGui_GetCursorStartPos),

    // Scrolling Functions
    JS_CFUNC_DEF("GetScrollX", 0, js_ImGui_GetScrollX),
    JS_CFUNC_DEF("GetScrollY", 0, js_ImGui_GetScrollY),
    JS_CFUNC_DEF("SetScrollX", 1, js_ImGui_SetScrollX),
    JS_CFUNC_DEF("SetScrollY", 1, js_ImGui_SetScrollY),
    JS_CFUNC_DEF("GetScrollMaxX", 0, js_ImGui_GetScrollMaxX),
    JS_CFUNC_DEF("GetScrollMaxY", 0, js_ImGui_GetScrollMaxY),
    JS_CFUNC_DEF("SetScrollHereX", 0, js_ImGui_SetScrollHereX),
    JS_CFUNC_DEF("SetScrollHereY", 0, js_ImGui_SetScrollHereY),

    // Menu Functions
    JS_CFUNC_DEF("BeginMenuBar", 0, js_ImGui_BeginMenuBar),
    JS_CFUNC_DEF("EndMenuBar", 0, js_ImGui_EndMenuBar),
    JS_CFUNC_DEF("BeginMainMenuBar", 0, js_ImGui_BeginMainMenuBar),
    JS_CFUNC_DEF("EndMainMenuBar", 0, js_ImGui_EndMainMenuBar),
    JS_CFUNC_DEF("BeginMenu", 2, js_ImGui_BeginMenu),
    JS_CFUNC_DEF("EndMenu", 0, js_ImGui_EndMenu),
    JS_CFUNC_DEF("MenuItem", 2, js_ImGui_MenuItem),

    // Popup and Modal Functions
    JS_CFUNC_DEF("BeginPopup", 2, js_ImGui_BeginPopup),
    JS_CFUNC_DEF("BeginPopupModal", 2, js_ImGui_BeginPopupModal),
    JS_CFUNC_DEF("EndPopup", 0, js_ImGui_EndPopup),
    JS_CFUNC_DEF("OpenPopup", 2, js_ImGui_OpenPopup),
    JS_CFUNC_DEF("OpenPopupOnItemClick", 2, js_ImGui_OpenPopupOnItemClick),
    JS_CFUNC_DEF("CloseCurrentPopup", 0, js_ImGui_CloseCurrentPopup),
    JS_CFUNC_DEF("BeginPopupContextItem", 2, js_ImGui_BeginPopupContextItem),
    JS_CFUNC_DEF("BeginPopupContextWindow", 2,
                 js_ImGui_BeginPopupContextWindow),
    JS_CFUNC_DEF("BeginPopupContextVoid", 2, js_ImGui_BeginPopupContextVoid),
    JS_CFUNC_DEF("IsPopupOpen", 2, js_ImGui_IsPopupOpen),

    // Tooltip Functions
    JS_CFUNC_DEF("BeginTooltip", 0, js_ImGui_BeginTooltip),
    JS_CFUNC_DEF("EndTooltip", 0, js_ImGui_EndTooltip),
    JS_CFUNC_DEF("SetTooltip", 1, js_ImGui_SetTooltip),
    JS_CFUNC_DEF("BeginItemTooltip", 0, js_ImGui_BeginItemTooltip),
    JS_CFUNC_DEF("SetItemTooltip", 1, js_ImGui_SetItemTooltip),

    // Window Functions
    JS_CFUNC_DEF("Begin", 2, js_ImGui_Begin),
    JS_CFUNC_DEF("End", 0, js_ImGui_End),
    JS_CFUNC_DEF("BeginChild", 2, js_ImGui_BeginChild),
    JS_CFUNC_DEF("EndChild", 0, js_ImGui_EndChild),

    // Window Utilities
    JS_CFUNC_DEF("IsWindowAppearing", 0, js_ImGui_IsWindowAppearing),
    JS_CFUNC_DEF("IsWindowCollapsed", 0, js_ImGui_IsWindowCollapsed),
    JS_CFUNC_DEF("IsWindowFocused", 1, js_ImGui_IsWindowFocused),
    JS_CFUNC_DEF("IsWindowHovered", 1, js_ImGui_IsWindowHovered),
    JS_CFUNC_DEF("GetWindowPos", 0, js_ImGui_GetWindowPos),
    JS_CFUNC_DEF("GetWindowSize", 0, js_ImGui_GetWindowSize),
    JS_CFUNC_DEF("GetWindowWidth", 0, js_ImGui_GetWindowWidth),
    JS_CFUNC_DEF("GetWindowHeight", 0, js_ImGui_GetWindowHeight),

    // Window Manipulation
    JS_CFUNC_DEF("SetNextWindowPos", 2, js_ImGui_SetNextWindowPos),
    JS_CFUNC_DEF("SetNextWindowSize", 2, js_ImGui_SetNextWindowSize),
    JS_CFUNC_DEF("SetNextWindowCollapsed", 2, js_ImGui_SetNextWindowCollapsed),
    JS_CFUNC_DEF("SetNextWindowFocus", 0, js_ImGui_SetNextWindowFocus),
    JS_CFUNC_DEF("SetNextWindowBgAlpha", 1, js_ImGui_SetNextWindowBgAlpha),

    // Style Functions
    JS_CFUNC_DEF("PushStyleColor", 2, js_ImGui_PushStyleColor),
    JS_CFUNC_DEF("PopStyleColor", 0, js_ImGui_PopStyleColor),
    JS_CFUNC_DEF("PushStyleVar", 2, js_ImGui_PushStyleVar),
    JS_CFUNC_DEF("PushStyleVarX", 2, js_ImGui_PushStyleVarX),
    JS_CFUNC_DEF("PushStyleVarY", 2, js_ImGui_PushStyleVarY),
    JS_CFUNC_DEF("PopStyleVar", 0, js_ImGui_PopStyleVar),

    // Item Functions
    JS_CFUNC_DEF("PushItemFlag", 2, js_ImGui_PushItemFlag),
    JS_CFUNC_DEF("PopItemFlag", 0, js_ImGui_PopItemFlag),
    JS_CFUNC_DEF("PushItemWidth", 1, js_ImGui_PushItemWidth),
    JS_CFUNC_DEF("PopItemWidth", 0, js_ImGui_PopItemWidth),
    JS_CFUNC_DEF("SetNextItemWidth", 1, js_ImGui_SetNextItemWidth),
    JS_CFUNC_DEF("CalcItemWidth", 0, js_ImGui_CalcItemWidth),
    JS_CFUNC_DEF("PushTextWrapPos", 0, js_ImGui_PushTextWrapPos),
    JS_CFUNC_DEF("PopTextWrapPos", 0, js_ImGui_PopTextWrapPos),

    // Font Functions
    JS_CFUNC_DEF("PushFont", 0, js_ImGui_PushFont),
    JS_CFUNC_DEF("PopFont", 0, js_ImGui_PopFont),
    JS_CFUNC_DEF("GetFont", 0, js_ImGui_GetFont),
    JS_CFUNC_DEF("GetFontSize", 0, js_ImGui_GetFontSize),
    JS_CFUNC_DEF("GetFontTexUvWhitePixel", 0, js_ImGui_GetFontTexUvWhitePixel),

    // Color Functions
    JS_CFUNC_DEF("GetColorU32", 1, js_ImGui_GetColorU32),
    JS_CFUNC_DEF("GetStyleColorVec4", 1, js_ImGui_GetStyleColorVec4),

    // Table Creation
    JS_CFUNC_DEF("BeginTable", 3, js_ImGui_BeginTable),
    JS_CFUNC_DEF("EndTable", 0, js_ImGui_EndTable),
    JS_CFUNC_DEF("TableNextRow", 1, js_ImGui_TableNextRow),
    JS_CFUNC_DEF("TableNextColumn", 0, js_ImGui_TableNextColumn),
    JS_CFUNC_DEF("TableSetColumnIndex", 1, js_ImGui_TableSetColumnIndex),

    // Table Setup
    JS_CFUNC_DEF("TableSetupColumn", 2, js_ImGui_TableSetupColumn),
    JS_CFUNC_DEF("TableSetupScrollFreeze", 2, js_ImGui_TableSetupScrollFreeze),
    JS_CFUNC_DEF("TableHeader", 1, js_ImGui_TableHeader),
    JS_CFUNC_DEF("TableHeadersRow", 0, js_ImGui_TableHeadersRow),
    JS_CFUNC_DEF("TableAngledHeadersRow", 0, js_ImGui_TableAngledHeadersRow),

    // Table Query
    JS_CFUNC_DEF("TableGetColumnCount", 0, js_ImGui_TableGetColumnCount),
    JS_CFUNC_DEF("TableGetColumnIndex", 0, js_ImGui_TableGetColumnIndex),
    JS_CFUNC_DEF("TableGetRowIndex", 0, js_ImGui_TableGetRowIndex),
    JS_CFUNC_DEF("TableGetColumnName", 1, js_ImGui_TableGetColumnName),
    JS_CFUNC_DEF("TableGetColumnFlags", 1, js_ImGui_TableGetColumnFlags),
    JS_CFUNC_DEF("TableSetBgColor", 3, js_ImGui_TableSetBgColor),

    // Table Sorting
    JS_CFUNC_DEF("TableGetSortSpecs", 0, js_ImGui_TableGetSortSpecs),

    // Item/Widget Queries
    JS_CFUNC_DEF("IsItemHovered", 0, js_ImGui_IsItemHovered),
    JS_CFUNC_DEF("IsItemActive", 0, js_ImGui_IsItemActive),
    JS_CFUNC_DEF("IsItemFocused", 0, js_ImGui_IsItemFocused),
    JS_CFUNC_DEF("IsItemClicked", 0, js_ImGui_IsItemClicked),
    JS_CFUNC_DEF("IsItemVisible", 0, js_ImGui_IsItemVisible),
    JS_CFUNC_DEF("IsItemEdited", 0, js_ImGui_IsItemEdited),
    JS_CFUNC_DEF("IsItemActivated", 0, js_ImGui_IsItemActivated),
    JS_CFUNC_DEF("IsItemDeactivated", 0, js_ImGui_IsItemDeactivated),
    JS_CFUNC_DEF("IsItemDeactivatedAfterEdit", 0,
                 js_ImGui_IsItemDeactivatedAfterEdit),
    JS_CFUNC_DEF("IsItemToggledOpen", 0, js_ImGui_IsItemToggledOpen),
    JS_CFUNC_DEF("IsAnyItemHovered", 0, js_ImGui_IsAnyItemHovered),
    JS_CFUNC_DEF("IsAnyItemActive", 0, js_ImGui_IsAnyItemActive),
    JS_CFUNC_DEF("IsAnyItemFocused", 0, js_ImGui_IsAnyItemFocused),
    JS_CFUNC_DEF("GetItemRectMin", 0, js_ImGui_GetItemRectMin),
    JS_CFUNC_DEF("GetItemRectMax", 0, js_ImGui_GetItemRectMax),
    JS_CFUNC_DEF("GetItemRectSize", 0, js_ImGui_GetItemRectSize),

    // Keyboard/Mouse/Gamepad Queries
    JS_CFUNC_DEF("IsKeyDown", 1, js_ImGui_IsKeyDown),
    JS_CFUNC_DEF("IsKeyPressed", 1, js_ImGui_IsKeyPressed),
    JS_CFUNC_DEF("IsKeyReleased", 1, js_ImGui_IsKeyReleased),
    JS_CFUNC_DEF("IsMouseDown", 1, js_ImGui_IsMouseDown),
    JS_CFUNC_DEF("IsMouseClicked", 1, js_ImGui_IsMouseClicked),
    JS_CFUNC_DEF("IsMouseReleased", 1, js_ImGui_IsMouseReleased),
    JS_CFUNC_DEF("IsMouseDoubleClicked", 1, js_ImGui_IsMouseDoubleClicked),
    JS_CFUNC_DEF("IsMouseDragging", 1, js_ImGui_IsMouseDragging),
    JS_CFUNC_DEF("GetMousePos", 0, js_ImGui_GetMousePos),
    JS_CFUNC_DEF("GetMousePosOnOpeningCurrentPopup", 0,
                 js_ImGui_GetMousePosOnOpeningCurrentPopup),
    JS_CFUNC_DEF("GetMouseDragDelta", 0, js_ImGui_GetMouseDragDelta),
    JS_CFUNC_DEF("ResetMouseDragDelta", 0, js_ImGui_ResetMouseDragDelta),

    // Clipboard
    JS_CFUNC_DEF("GetClipboardText", 0, js_ImGui_GetClipboardText),
    JS_CFUNC_DEF("SetClipboardText", 1, js_ImGui_SetClipboardText),

    // Miscellaneous Utilities
    JS_CFUNC_DEF("SetItemDefaultFocus", 0, js_ImGui_SetItemDefaultFocus),
    JS_CFUNC_DEF("SetKeyboardFocusHere", 0, js_ImGui_SetKeyboardFocusHere),
    JS_CFUNC_DEF("Shortcut", 1, js_ImGui_Shortcut),
    JS_CFUNC_DEF("SetNextItemShortcut", 1, js_ImGui_SetNextItemShortcut),
    JS_CFUNC_DEF("SetNextItemAllowOverlap", 0,
                 js_ImGui_SetNextItemAllowOverlap),
    JS_CFUNC_DEF("BeginDisabled", 0, js_ImGui_BeginDisabled),
    JS_CFUNC_DEF("EndDisabled", 0, js_ImGui_EndDisabled),

    // Tree Nodes
    JS_CFUNC_DEF("TreeNode", 1, js_ImGui_TreeNode),
    JS_CFUNC_DEF("TreeNodeV", 2, js_ImGui_TreeNodeV),
    JS_CFUNC_DEF("TreeNodeEx", 1, js_ImGui_TreeNodeEx),
    JS_CFUNC_DEF("TreeNodeEx2", 3, js_ImGui_TreeNodeEx2),
    JS_CFUNC_DEF("TreePush", 1, js_ImGui_TreePush),
    JS_CFUNC_DEF("TreePushPtr", 1, js_ImGui_TreePushPtr),
    JS_CFUNC_DEF("TreePop", 0, js_ImGui_TreePop),
    JS_CFUNC_DEF("GetTreeNodeToLabelSpacing", 0,
                 js_ImGui_GetTreeNodeToLabelSpacing),
    JS_CFUNC_DEF("CollapsingHeader", 1, js_ImGui_CollapsingHeader),
    JS_CFUNC_DEF("CollapsingHeaderEx", 2, js_ImGui_CollapsingHeaderEx),
    JS_CFUNC_DEF("SetNextItemOpen", 1, js_ImGui_SetNextItemOpen),

    // Selectables
    JS_CFUNC_DEF("Selectable", 1, js_ImGui_Selectable),
    JS_CFUNC_DEF("SelectableEx", 2, js_ImGui_SelectableEx),

    // List Boxes
    JS_CFUNC_DEF("BeginListBox", 1, js_ImGui_BeginListBox),
    JS_CFUNC_DEF("EndListBox", 0, js_ImGui_EndListBox),
    JS_CFUNC_DEF("ListBox", 3, js_ImGui_ListBox),

    // Combos
    JS_CFUNC_DEF("BeginCombo", 2, js_ImGui_BeginCombo),
    JS_CFUNC_DEF("EndCombo", 0, js_ImGui_EndCombo),
    JS_CFUNC_DEF("Combo", 3, js_ImGui_Combo),
    JS_CFUNC_DEF("ComboSimple", 3, js_ImGui_ComboSimple),
};

} // namespace

// Deliberately not a module. Every function here is only valid between
// NewFrame and Render, which is the window the host calls draw_gui in, so the
// object is handed to that one callback instead of being importable from
// anywhere. JS_SetPropertyFunctionList is also the only instantiation path that
// honours JS_DEF_CGETSET, should this list ever grow one - JS_SetModuleExportList
// ends in `default: abort()`.
JSValue js_imgui_new_namespace(JSContext *ctx) {
  JSValue ns = JS_NewObject(ctx);
  if (JS_IsException(ns)) {
    return ns;
  }
  if (JS_SetPropertyFunctionList(ctx, ns, js_imgui_funcs,
                                 countof(js_imgui_funcs)) < 0) {
    JS_FreeValue(ctx, ns);
    return JS_EXCEPTION;
  }
  return ns;
}