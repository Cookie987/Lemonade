--- uikit.lua — declarative UI for LemonadeOS Lua apps.
---
--- Apps describe the UI as a tree of specs and never touch widget identity directly:
---
---   { kind = "obj", key = "root", style = {...}, props = {...},
---     children = { { kind = "label", key = "title", props = { text = "Hi" } } },
---     on = { [lv.EVENT_CLICKED] = function(event, node) ... end } }
---
--- uikit diffs the new tree against the previous one and applies only the changes, all
--- inside one LVGL batch. The runtime pauses rendering during a batch, so the update lands
--- as one transaction — this is the app-facing half of the UI actor: app logic runs on the
--- Lua task, the UI task owns all widgets.
---
--- Typical app:
---
---   local uikit = require("uikit")
---   local app = uikit.app({ state = {...}, view = function(state) return spec end })
---   app.mount(page)
---   sys.timerLoopStart(function() lv.poll_events(10); app.tick() end, 16)

local lv = lvgl

local M = {}

local TYPE_ALIASES = {
  image = "img",
  button = "btn",
}

local function kind_name(kind)
  kind = kind or "obj"
  return TYPE_ALIASES[kind] or kind
end

local function same_table(a, b)
  if a == b then
    return true
  end
  if type(a) ~= "table" or type(b) ~= "table" then
    return false
  end
  for k, v in pairs(a) do
    if b[k] ~= v then
      return false
    end
  end
  for k, v in pairs(b) do
    if a[k] ~= v then
      return false
    end
  end
  return true
end

local function same_array(a, b)
  if a == b then
    return true
  end
  if type(a) ~= "table" or type(b) ~= "table" or #a ~= #b then
    return false
  end
  for i = 1, #a do
    if a[i] ~= b[i] then
      return false
    end
  end
  return true
end

local function set_flag(obj, flag, enabled)
  if enabled then
    lv.obj_add_flag(obj, flag)
  else
    lv.obj_clear_flag(obj, flag)
  end
end

-- Style keys. Those with a selector pass 0 (LV_PART_MAIN); x/y/width/height/align are
-- geometry and are handled separately.
local STYLE_SETTERS = {
  bg_color = function(o, v, p) lv.obj_set_style_bg_color(o, v, p) end,
  bg_opa = function(o, v, p) lv.obj_set_style_bg_opa(o, v, p) end,
  bg_grad_color = function(o, v, p) lv.obj_set_style_bg_grad_color(o, v, p) end,
  bg_grad_dir = function(o, v, p) lv.obj_set_style_bg_grad_dir(o, v, p) end,
  text_color = function(o, v, p) lv.obj_set_style_text_color(o, v, p) end,
  text_font = function(o, v, p) lv.obj_set_style_text_font(o, v, p) end,
  text_align = function(o, v, p) lv.obj_set_style_text_align(o, v, p) end,
  text_line_space = function(o, v, p) lv.obj_set_style_text_line_space(o, v, p) end,
  radius = function(o, v, p) lv.obj_set_style_radius(o, v, p) end,
  border_width = function(o, v, p) lv.obj_set_style_border_width(o, v, p) end,
  border_color = function(o, v, p) lv.obj_set_style_border_color(o, v, p) end,
  outline_width = function(o, v, p) lv.obj_set_style_outline_width(o, v, p) end,
  pad_all = function(o, v, p) lv.obj_set_style_pad_all(o, v, p) end,
  pad_top = function(o, v, p) lv.obj_set_style_pad_top(o, v, p) end,
  pad_bottom = function(o, v, p) lv.obj_set_style_pad_bottom(o, v, p) end,
  pad_left = function(o, v, p) lv.obj_set_style_pad_left(o, v, p) end,
  pad_right = function(o, v, p) lv.obj_set_style_pad_right(o, v, p) end,
  pad_row = function(o, v, p) lv.obj_set_style_pad_row(o, v, p) end,
  pad_column = function(o, v, p) lv.obj_set_style_pad_column(o, v, p) end,
  shadow_width = function(o, v, p) lv.obj_set_style_shadow_width(o, v, p) end,
  shadow_color = function(o, v, p) lv.obj_set_style_shadow_color(o, v, p) end,
}

local function apply_style(node, style, part)
  local obj = node.obj
  part = part or 0
  for key, value in pairs(style or {}) do
    if key == "x" then
      lv.obj_set_x(obj, value)
    elseif key == "y" then
      lv.obj_set_y(obj, value)
    elseif key == "width" then
      lv.obj_set_width(obj, value)
    elseif key == "height" then
      lv.obj_set_height(obj, value)
    elseif key == "align" then
      lv.obj_align(obj, value[1], value[2] or 0, value[3] or 0)
    else
      local setter = STYLE_SETTERS[key]
      if setter then
        setter(obj, value, part)
      end
    end
  end
end

-- Per-kind property setters. Anything else is treated as an object flag / layout property
-- in apply_props below. Buttons get an implicit centered label for props.text.
local PROP_SETTERS = {
  label = {
    text = function(o, v) lv.label_set_text(o, v) end,
    long_mode = function(o, v) lv.label_set_long_mode(o, v) end,
    recolor = function(o, v) lv.label_set_recolor(o, v) end,
  },
  textarea = {
    text = function(o, v) lv.textarea_set_text(o, v) end,
    placeholder = function(o, v) lv.textarea_set_placeholder_text(o, v) end,
    one_line = function(o, v) lv.textarea_set_one_line(o, v) end,
  },
  img = {
    src = function(o, v) lv.img_set_src(o, v) end,
  },
  bar = {
    value = function(o, v) lv.bar_set_value(o, v, false) end,
    range = function(o, v) lv.bar_set_range(o, v[1], v[2]) end,
  },
  dropdown = {
    options = function(o, v)
      if type(v) == "table" then
        v = table.concat(v, "\n")
      end
      lv.dropdown_set_options(o, v)
    end,
    selected = function(o, v) lv.dropdown_set_selected(o, v) end,
  },
  keyboard = {
    textarea = function(o, v) lv.keyboard_set_textarea(o, v) end,
  },
}

local function apply_props(node, props)
  local obj = node.obj
  -- Range first: setting value before range would clamp it to the old/default range.
  if node.kind == "bar" and props and props.range then
    lv.bar_set_range(obj, props.range[1], props.range[2])
  end
  for key, value in pairs(props or {}) do
    if key == "hidden" then
      set_flag(obj, lv.FLAG_HIDDEN, value)
    elseif key == "clickable" then
      set_flag(obj, lv.FLAG_CLICKABLE, value)
    elseif key == "scrollable" then
      set_flag(obj, lv.FLAG_SCROLLABLE, value)
    elseif key == "scroll_dir" then
      lv.obj_set_scroll_dir(obj, value)
    elseif key == "scrollbar_mode" then
      lv.obj_set_scrollbar_mode(obj, value)
    elseif key == "flex_flow" then
      lv.obj_set_flex_flow(obj, value)
    elseif key == "flex_align" then
      lv.obj_set_flex_align(obj, value[1], value[2], value[3])
    elseif key == "ignore_layout" then
      set_flag(obj, lv.FLAG_IGNORE_LAYOUT, value)
    elseif node.kind == "bar" and key == "range" then
      -- already applied above
    elseif node.kind == "btn" and key == "text" then
      if not node.text_label then
        node.text_label = lv.label_create(obj)
        lv.obj_center(node.text_label)
      end
      lv.label_set_text(node.text_label, value)
    else
      local setters = PROP_SETTERS[node.kind]
      local setter = setters and setters[key]
      if setter then
        setter(obj, value)
      end
    end
  end
end

local function bind_events(node)
  if node.bound then
    return
  end
  node.bound = true
  lv.obj_add_event_cb(node.obj, function(event)
    local spec = node.spec
    local handler = spec and spec.on and spec.on[lv.event_get_code(event)]
    if handler then
      handler(event, node)
    end
  end, lv.EVENT_ALL)
end

local function create_node(parent, spec)
  local kind = kind_name(spec.kind)
  local create = lv[kind .. "_create"]
  if type(create) ~= "function" then
    error("uikit: unknown kind '" .. tostring(spec.kind) .. "'")
  end
  local obj = create(parent)
  return {
    obj = obj,
    kind = kind,
    key = spec.key,
    spec = spec,
    children = {},
  }
end

local function apply_node(parent, spec, old_node, existing_obj)
  local kind = kind_name(spec.kind)
  local node = old_node

  if existing_obj ~= nil then
    -- The app page itself is the root: reuse it instead of adding an extra layer, which
    -- would inherit the page's layout/padding and shift every child.
    kind = "obj"
    if node == nil then
      node = { obj = existing_obj, kind = "obj", key = spec.key, spec = spec, children = {} }
    end
  else
    -- A different kind or a different explicit key cannot be reused.
    if node and (node.kind ~= kind or (spec.key ~= nil and node.key ~= spec.key)) then
      lv.obj_del(node.obj)
      node = nil
    end

    if not node then
      node = create_node(parent, spec)
    end
  end
  node.kind = kind
  node.key = spec.key
  node.spec = spec

  if not same_table(node.props, spec.props) then
    apply_props(node, spec.props)
    node.props = spec.props
  end
  if not same_table(node.style, spec.style) then
    apply_style(node, spec.style)
    node.style = spec.style
  end
  if spec.parts then
    node.part_styles = node.part_styles or {}
    for part, part_style in pairs(spec.parts) do
      if not same_table(node.part_styles[part], part_style) then
        apply_style(node, part_style, part)
        node.part_styles[part] = part_style
      end
    end
  end
  if spec.on then
    bind_events(node)
  end

  local next_children = {}
  local order = {}
  local children = spec.children or {}
  for index, child_spec in ipairs(children) do
    local child_key = child_spec.key or ("@" .. index)
    local child = apply_node(node.obj, child_spec, node.children[child_key])
    next_children[child_key] = child
    order[#order + 1] = child_key
  end
  -- Only pay for reordering when the child order/key set actually changed.
  if not same_array(node.order, order) then
    for index, child_key in ipairs(order) do
      lv.obj_move_to_index(next_children[child_key].obj, index - 1)
    end
    node.order = order
  end
  for child_key, child in pairs(node.children) do
    if not next_children[child_key] then
      lv.obj_del(child.obj)
    end
  end
  node.children = next_children

  if type(spec.ref) == "function" then
    spec.ref(node.obj, node)
  end

  return node
end

--- Creates a declarative app.
-- opts.state: initial state table (mutated in place by set_state).
-- opts.view(state): returns the spec tree for the current state.
function M.app(opts)
  assert(type(opts) == "table", "uikit.app: opts table required")
  assert(type(opts.view) == "function", "uikit.app: opts.view function required")

  local app = {
    state = opts.state or {},
    view = opts.view,
    dirty = true,
    parent = nil,
    root = nil,
  }

  function app.set_state(patch)
    if type(patch) == "table" then
      for key, value in pairs(patch) do
        app.state[key] = value
      end
    end
    app.dirty = true
  end

  function app.render()
    if app.parent == nil then
      return
    end
    app.dirty = false
    local spec = app.view(app.state)
    lv.batch_begin()
    local ok, err = pcall(function()
      app.root = apply_node(app.parent, spec, app.root, app.parent)
    end)
    lv.batch_end()
    if not ok then
      error(err)
    end
  end

  function app.tick()
    if app.dirty then
      app.render()
    end
  end

  function app.mount(parent)
    app.parent = parent
    app.root = nil
    app.render()
  end

  function app.unmount()
    if app.root and app.parent and app.root.obj ~= app.parent then
      lv.obj_del(app.root.obj)
    end
    app.root = nil
    app.parent = nil
  end

  return app
end

return M
