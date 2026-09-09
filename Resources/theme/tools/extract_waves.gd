extends Node
# Pull the frames out of waves.ogv and pack them into one sprite sheet.
#
# WHY GODOT DOES THIS. The file is Ogg Theora. There is no ffmpeg on this
# machine and the original mp4 is gone, so Godot, which owns the format, is the
# only thing here that can read it at all. It runs once, offline, and what ships
# is the sheet.
#
# WHY A SHEET AND NOT FRAMES. 192 PNG frames is 29 MB, which would be fifteen
# times the whole add-on package for a backdrop that sits under a 0.72 dim and
# is barely visible. One sheet, at half the frame rate and a third of the size,
# is about a megabyte and looks identical once dimmed. Slate draws it by moving
# a UV window over a single texture, so it is also one allocation rather than
# 192.
#
# EVERY SECOND FRAME. The source is 24 fps; the loop is water, not motion that
# needs the extra frames, and 12 fps halves both the sheet and the memory.

const CELL_W := 160
const CELL_H := 267
const COLS := 12
const ROWS := 8
const KEEP_EVERY := 2          # 24 fps source, 12 fps sheet
const MAX_CELLS := COLS * ROWS

var _player: VideoStreamPlayer
var _seen := 0                 # decoded frames, including the ones dropped
var _n := 0                    # cells written
var _last: PackedByteArray = PackedByteArray()
var _same := 0
var _sheet: Image
var _out := ""

func _ready() -> void:
	_out = OS.get_environment("WAVES_OUT")
	if _out == "":
		_out = "res://waves_sheet.jpg"
	_sheet = Image.create(CELL_W * COLS, CELL_H * ROWS, false, Image.FORMAT_RGB8)
	_sheet.fill(Color(0, 0, 0))

	var s := VideoStreamTheora.new()
	s.file = "res://waves.ogv"
	_player = VideoStreamPlayer.new()
	_player.stream = s
	_player.expand = false
	_player.loop = false
	add_child(_player)
	_player.play()
	print("WAVESDUMP start")

func _process(_d: float) -> void:
	if _player == null:
		return
	if not _player.is_playing() and _n > 0:
		_done("stream ended")
		return
	var tex := _player.get_video_texture()
	if tex == null:
		return
	var img := tex.get_image()
	if img == null:
		return

	# The same bytes twice means the decoder has not advanced. A long run of
	# them means it has stopped for good.
	var cur := img.get_data()
	if cur == _last:
		_same += 1
		if _same > 120:
			_done("no new frames")
		return
	_same = 0
	_last = cur

	# The very first texture is the decoder warming up and comes out empty.
	_seen += 1
	if _seen <= 1:
		return
	if (_seen % KEEP_EVERY) != 0:
		return
	if _n >= MAX_CELLS:
		_done("sheet full")
		return

	img.convert(Image.FORMAT_RGB8)
	img.resize(CELL_W, CELL_H, Image.INTERPOLATE_BILINEAR)
	var col := _n % COLS
	var row := _n / COLS
	_sheet.blit_rect(img, Rect2i(0, 0, CELL_W, CELL_H),
		Vector2i(col * CELL_W, row * CELL_H))
	_n += 1

func _done(why: String) -> void:
	_player = null
	# Quality is generous for a photographic loop and still small, and the dim
	# in front of it hides what little the encoder gives up.
	var err := _sheet.save_jpg(_out, 0.86)
	print("WAVESDUMP done: %d cells of %dx%d in %dx%d (%s), save=%d" %
		[_n, CELL_W, CELL_H, CELL_W * COLS, CELL_H * ROWS, why, err])
	# The loop has to be seamless, so the panel needs to know how many cells are
	# real: the tail of the sheet is black padding.
	var f := FileAccess.open(_out.get_basename() + ".json", FileAccess.WRITE)
	if f != null:
		f.store_string(JSON.stringify({
			"cells": _n, "cols": COLS, "rows": ROWS,
			"cell_w": CELL_W, "cell_h": CELL_H, "fps": 12,
			"source": "waves.ogv", "note":
				"Frames of the Godot plugin's waves backdrop, every second frame at 12 fps. "
				+ "Rebuild with the extractor in the session scratchpad if the video changes."
		}, "  "))
		f.close()
	get_tree().quit()
