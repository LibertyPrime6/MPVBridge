local mp = require 'mp'
local options = require 'mp.options'
local utils = require 'mp.utils'

local o = {
    site = '',
}

options.read_options(o, 'external_player_ytdl_labels')

local function round(value)
    return math.floor((tonumber(value) or 0) + 0.5)
end

local function edl_escape(value)
    value = tostring(value or '')
    return '%' .. #value .. '%' .. value
end

local function codec_name(codec)
    codec = tostring(codec or ''):lower()
    if codec == '' or codec == 'null' or codec == 'none' then return '' end
    if codec:find('av1', 1, true) or codec:find('av01', 1, true) then return 'AV1' end
    if codec:find('vp9', 1, true) or codec:find('vp09', 1, true) then return 'VP9' end
    if codec:find('dvh1', 1, true) or codec:find('dvhe', 1, true) or
        codec:find('hev1', 1, true) or codec:find('hvc1', 1, true) or
        codec:find('hevc', 1, true) or codec:find('h265', 1, true) then return 'HEVC' end
    if codec:find('h264', 1, true) or codec:find('avc', 1, true) then return 'AVC' end
    if codec:find('eac3', 1, true) or codec:find('ec%-3') then return 'E-AC-3' end
    if codec:find('ac3', 1, true) then return 'AC-3' end
    if codec:find('flac', 1, true) then return 'FLAC' end
    if codec:find('opus', 1, true) then return 'Opus' end
    if codec:find('aac', 1, true) or codec:find('mp4a', 1, true) then return 'AAC / M4A' end
    return codec:upper()
end

local function video_base_name(height)
    height = round(height)
    if height >= 4320 then return '8K' end
    if height >= 2160 then return '4K' end
    if height > 0 then return tostring(height) .. 'P' end
    return '视频'
end

local function bilibili_video_title(old_title, properties)
    local lower = old_title:lower()
    local dynamic_range = tostring(properties.dynamic_range or ''):lower()
    local height = round(properties.h)
    local fps = round(properties.fps)
    local base
    if old_title:find('杜比视界', 1, true) or lower:find('dolby', 1, true) or
        dynamic_range:find('dolby', 1, true) or dynamic_range == 'dv' then
        base = '杜比视界'
    elseif old_title:find('HDR', 1, true) or lower:find('hdr', 1, true) or
        dynamic_range:find('hdr', 1, true) or dynamic_range:find('hlg', 1, true) then
        base = 'HDR 真彩'
    elseif old_title:find('8K', 1, true) or height >= 4320 then
        base = '8K 超高清'
    elseif old_title:find('4K', 1, true) or height >= 2160 then
        base = '4K 超清'
    elseif height >= 1080 and fps >= 50 then
        base = '1080P 60帧'
    elseif old_title:find('高码率', 1, true) or old_title:find('1080P+', 1, true) then
        base = '1080P 高码率'
    elseif height >= 1080 then
        base = '1080P 高清'
    elseif height >= 720 and fps >= 50 then
        base = '720P 60帧'
    elseif height >= 720 then
        base = '720P 高清'
    elseif height >= 480 then
        base = '480P 清晰'
    elseif height > 0 then
        base = tostring(height) .. 'P'
    else
        base = old_title ~= '' and old_title or '视频'
    end
    local codec = codec_name(properties.codec)
    return codec ~= '' and (base .. ' · ' .. codec) or base
end

local function youtube_video_title(old_title, properties)
    local base = video_base_name(properties.h)
    local fps = round(properties.fps)
    if fps >= 50 then
        base = base .. ' ' .. tostring(fps) .. 'FPS'
    end
    local lower = old_title:lower()
    local dynamic_range = tostring(properties.dynamic_range or ''):lower()
    if dynamic_range:find('hdr', 1, true) or lower:find('hdr', 1, true) then
        base = base .. ' HDR'
    elseif dynamic_range:find('hlg', 1, true) or lower:find('hlg', 1, true) then
        base = base .. ' HLG'
    end
    local codec = codec_name(properties.codec)
    return codec ~= '' and (base .. ' · ' .. codec) or base
end

local function bilibili_audio_title(old_title, properties, kbps)
    local codec = codec_name(properties.codec)
    local lower = old_title:lower()
    if codec == 'E-AC-3' or codec == 'AC-3' or lower:find('dolby', 1, true) or
        old_title:find('杜比', 1, true) then
        return '杜比全景声'
    end
    if codec == 'FLAC' or lower:find('flac', 1, true) or lower:find('hi%-res') then
        return 'Hi-Res 无损'
    end
    local format_id = tostring(properties.format_id or '')
    if format_id == '30280' or old_title:find('30280', 1, true) or kbps >= 160 then return '192K AAC' end
    if format_id == '30232' or old_title:find('30232', 1, true) or kbps >= 90 then return '132K AAC' end
    if format_id == '30216' or old_title:find('30216', 1, true) or kbps > 0 then return '64K AAC' end
    return codec ~= '' and codec or (old_title ~= '' and old_title or '音频')
end

local function youtube_audio_title(old_title, properties, kbps)
    local lower = old_title:lower()
    local codec = codec_name(properties.codec)
    local channels = tonumber(properties.channels) or 0
    local format_id = tostring(properties.format_id or ''):lower()
    local multichannel = channels >= 3 or lower:find('5%.1') or lower:find('spatial', 1, true) or
        lower:find('surround', 1, true) or codec == 'E-AC-3' or codec == 'AC-3'
    local base
    if multichannel then
        base = '多声道 / 空间音频'
    elseif format_id:find('%-drc$') or lower:find('drc', 1, true) then
        base = 'DRC 音轨'
    else
        base = '原始音轨'
    end
    if codec ~= '' then base = base .. ' · ' .. codec end
    if kbps > 0 then base = base .. ' · ' .. tostring(kbps) .. 'K' end
    return base
end

local function collect_properties(context)
    local properties = {}
    for key, value in context:gmatch('([%w_]+)=([^,;]+)') do
        properties[key] = value
    end
    return properties
end

local function make_title_from_properties(old_title, properties, kbps)
    if properties.media_type == 'video' then
        return o.site == 'bilibili' and bilibili_video_title(old_title, properties) or
            youtube_video_title(old_title, properties)
    end
    if properties.media_type == 'audio' then
        return o.site == 'bilibili' and bilibili_audio_title(old_title, properties, kbps) or
            youtube_audio_title(old_title, properties, kbps)
    end
    return old_title
end

local function make_title(old_title, context, meta_tail)
    local properties = collect_properties(context)
    local byterate = tonumber(meta_tail:match('byterate=(%d+)')) or 0
    return make_title_from_properties(old_title, properties, round(byterate * 8 / 1000))
end

local function rewrite_edl_titles(edl)
    local chunks = {}
    local cursor = 1
    local stream_start = 1
    local changed = 0
    while true do
        local marker_start, marker_end, length_text =
            edl:find('!track_meta,title=%%(%d+)%%', cursor)
        if not marker_start then break end

        local between = edl:sub(stream_start, marker_start - 1)
        local relative_stream_start = between:match('.*()!new_stream')
        if relative_stream_start then
            stream_start = stream_start + relative_stream_start - 1
        end

        local title_length = tonumber(length_text) or 0
        local title_start = marker_end + 1
        local title_end = title_start + title_length - 1
        if title_end > #edl then break end
        local meta_end = edl:find(';', title_end + 1, true) or (#edl + 1)
        local old_title = edl:sub(title_start, title_end)
        local context = edl:sub(stream_start, marker_start - 1)
        local meta_tail = edl:sub(title_end + 1, meta_end - 1)
        local new_title = make_title(old_title, context, meta_tail)

        chunks[#chunks + 1] = edl:sub(cursor, marker_start - 1)
        chunks[#chunks + 1] = '!track_meta,title=' .. edl_escape(new_title)
        cursor = title_end + 1
        changed = changed + (new_title ~= old_title and 1 or 0)
    end
    chunks[#chunks + 1] = edl:sub(cursor)
    return table.concat(chunks), changed
end

local function map_codec(codec, media_type)
    codec = tostring(codec or ''):lower()
    if media_type == 'video' then
        if codec:find('av01', 1, true) then return 'av1' end
        if codec:find('vp9', 1, true) or codec:find('vp09', 1, true) then return 'vp9' end
        -- Dolby Vision elementary streams use dvh1/dvhe sample-entry names.
        -- mpv's delayed EDL opener expects a decoder codec name here, not the
        -- container sample entry/profile string (for example dvh1.05.06).
        if codec:find('dvh1', 1, true) or codec:find('dvhe', 1, true) or
            codec:find('hev1', 1, true) or codec:find('hvc1', 1, true) or
            codec:find('hevc', 1, true) or codec:find('h265', 1, true) then return 'hevc' end
        if codec:find('avc1', 1, true) or codec:find('h264', 1, true) then return 'h264' end
    else
        if codec:find('mp4a', 1, true) or codec:find('aac', 1, true) then return 'aac' end
        if codec:find('ec%-3') or codec:find('eac3', 1, true) then return 'eac3' end
        if codec:find('ac%-3') or codec:find('ac3', 1, true) then return 'ac3' end
        if codec:find('flac', 1, true) then return 'flac' end
        if codec:find('opus', 1, true) then return 'opus' end
    end
    return codec:match('^[%w_.-]+') or 'null'
end

local function get_ytdl_info()
    local result = mp.get_property_native('user-data/mpv/ytdl/json-subprocess-result')
    if type(result) == 'string' then result = utils.parse_json(result) end
    if type(result) ~= 'table' then return nil end
    local stdout = result.stdout
    local info = type(stdout) == 'string' and utils.parse_json(stdout) or nil
    if type(info) ~= 'table' then return nil end
    return info
end

local function get_requested_formats(info)
    info = info or get_ytdl_info()
    if type(info) ~= 'table' then return nil end
    local formats = info.requested_formats or info.requested_downloads
    return type(formats) == 'table' and formats or nil
end

local function track_media_type(track)
    if track.vcodec and track.vcodec ~= 'none' then return 'video' end
    if track.acodec and track.acodec ~= 'none' then return 'audio' end
    return nil
end

local function track_byterate(track, media_type)
    local rate = media_type == 'video' and track.vbr or track.abr
    rate = tonumber(rate) or tonumber(track.tbr) or 0
    return math.floor(rate * 1000 / 8)
end

local function inject_requested_format_metadata(edl)
    local info = get_ytdl_info()
    local formats = get_requested_formats(info)
    if not formats then return edl, 0 end
    local duration = not info.is_live and tonumber(info.duration) or 0
    local duration_suffix = duration and duration > 0 and
        (',length=' .. string.format('%.6f', duration)) or ''
    local matches = {}
    local used_positions = {}
    for _, track in ipairs(formats) do
        local media_type = track_media_type(track)
        local url = track.url
        if media_type and type(url) == 'string' and url ~= '' then
            local needle = edl_escape(url)
            local position = edl:find(needle, 1, true)
            if position and not used_positions[position] then
                used_positions[position] = true
                matches[#matches + 1] = {
                    position = position,
                    needle_length = #needle,
                    media_type = media_type,
                    track = track,
                }
            end
        end
    end
    table.sort(matches, function(a, b) return a.position < b.position end)

    local first_type = {}
    for _, item in ipairs(matches) do
        local track = item.track
        local media_type = item.media_type
        local codec = map_codec(media_type == 'video' and track.vcodec or track.acodec, media_type)
        local properties = {
            media_type = media_type,
            codec = codec,
            h = track.height,
            w = track.width,
            fps = track.fps,
            samplerate = track.asr,
            channels = track.audio_channels,
            format_id = track.format_id,
            dynamic_range = track.dynamic_range,
        }
        local delay = '!delay_open,media_type=' .. media_type .. ',codec=' .. codec
        if media_type == 'video' then
            delay = delay .. ',w=' .. tostring(round(track.width)) ..
                ',h=' .. tostring(round(track.height)) .. ',fps=' .. tostring(round(track.fps))
        else
            delay = delay .. ',samplerate=' .. tostring(round(track.asr))
        end
        local byterate = track_byterate(track, media_type)
        local old_title = tostring(track.format or track.format_note or '')
        local title = make_title_from_properties(old_title, properties, round(byterate * 8 / 1000))
        local metadata = '!track_meta,title=' .. edl_escape(title) .. ',byterate=' .. tostring(byterate)
        if not first_type[media_type] then
            first_type[media_type] = true
            metadata = metadata .. ',flags=default'
        end
        item.insert = delay .. ';' .. metadata .. ';'
        item.duration_suffix = duration_suffix
    end

    for index = #matches, 1, -1 do
        local item = matches[index]
        local url_end = item.position + item.needle_length - 1
        local suffix = edl:sub(url_end + 1):find('^,length=') and '' or item.duration_suffix
        edl = edl:sub(1, item.position - 1) .. item.insert ..
            edl:sub(item.position, url_end) .. suffix .. edl:sub(url_end + 1)
    end
    return edl, #matches
end

local function rewrite_ytdl_edl()
    if o.site ~= 'bilibili' and o.site ~= 'youtube' then return end
    local path = mp.get_property('stream-open-filename', '')
    if not path:find('^edl://') then return end
    local rewritten, changed
    if path:find('!track_meta,title=', 1, true) then
        rewritten, changed = rewrite_edl_titles(path)
    else
        rewritten, changed = inject_requested_format_metadata(path)
    end
    if changed > 0 then
        mp.set_property('stream-open-filename', rewritten)
        mp.msg.verbose('Renamed ' .. tostring(changed) .. ' yt-dlp tracks for ' .. o.site)
    end
end

mp.add_hook('on_load', 30, rewrite_ytdl_edl)
-- Some sites (notably Bilibili) are handed to yt-dlp only after mpv's direct
-- URL open fails. Run after ytdl_hook's on_load_fail handler as well so the
-- delayed alternative tracks receive their titles, defaults and duration.
mp.add_hook('on_load_fail', 40, rewrite_ytdl_edl)
