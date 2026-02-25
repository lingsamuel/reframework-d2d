local cfg = json.load_file("reframework-d2d.json")
if not cfg then
    cfg = {}
end

if cfg.paper_white_nits == nil then
    cfg.paper_white_nits = 200.0
end

if cfg.max_update_rate == nil then
    cfg.max_update_rate = d2d.detail.get_max_updaterate()
end

re.on_config_save(
    function()
        json.dump_file("reframework-d2d.json", cfg)
    end
)

re.on_draw_ui(
    function()
        if not imgui.collapsing_header("REFramework D2D") then return end

        local changed, value = imgui.slider_int("Max Update Rate", cfg.max_update_rate, 1, 300)
        if changed then 
            cfg.max_update_rate = value 
        end

        local output_mode = d2d.detail.get_output_mode()
        local output_mode_name = "Unknown"
        if output_mode == 0 then
            output_mode_name = "SDR"
        elseif output_mode == 1 then
            output_mode_name = "HDR10 PQ"
        elseif output_mode == 2 then
            output_mode_name = "scRGB"
        end

        local is_hdr = d2d.detail.is_hdr()
        imgui.text(is_hdr and "HDR Output: On" or "HDR Output: Off")
        if is_hdr then
            imgui.text("Output Mode: " .. output_mode_name)
        end

        if is_hdr then
            local changed_pw, pw = imgui.slider_float("Paper White (nits)", cfg.paper_white_nits, 80.0, 1000.0)
            if changed_pw then
                cfg.paper_white_nits = pw
            end
        end

        local last_error = d2d.detail.get_last_error()
        if last_error ~= "" then
            imgui.text("Last Script Error:")
            imgui.text_colored(last_error, 0xFF0000FF)
        end
    end
)

d2d.register(
    function()
        if cfg.paper_white_nits == nil then
            cfg.paper_white_nits = d2d.detail.get_paper_white_nits()
        end
    end,
    function()
        d2d.detail.set_max_updaterate(cfg.max_update_rate)
        if d2d.detail.is_hdr() then
            d2d.detail.set_paper_white_nits(cfg.paper_white_nits)
        end
    end
)
