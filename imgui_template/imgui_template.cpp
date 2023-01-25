// imgui_template.cpp : Defines the entry point for the application.
//

#include "imgui_template.h"

// IMGUI and glfw includes
#define IMGUI_IMPLEMENTATION
#include <gl/glew.h>
#include <GLFW/glfw3.h>
#include "imgui/imgui.h"
#include "imgui/imgui_stdlib.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"
//we need this to change tesselation tolerance
#include "imgui/imgui_internal.h"
#include "zpp_bits/zpp_bits.h"
#include "rpc/server.h"
#include "rpc/client.h"
#include "rpc/rpc_error.h"
#include "sqlite3.h"

#include "stack_vector/stack_vector.h"
#include "fmt/format.h"
//#include "rpc/dispatcher.h"

#include <thread>
#include <bit>
#include <span>

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

struct glfw3_setup_t {
	int err_code;
	GLFWwindow* window;
};

glfw3_setup_t glfw3_setup(uint32_t default_window_width, uint32_t default_window_height, bool fullscreen = false) {
	// Setup window
	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit())
		return { 1,nullptr };

	//vg::Context svg_ctx;
	// Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
	// GL ES 2.0 + GLSL 100
	const char* glsl_version = "#version 100";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
	// GL 3.2 + GLSL 150
	const char* glsl_version = "#version 150";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
	// GL 3.0 + GLSL 130
	const char* glsl_version = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	//glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
	//glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only
#endif
	//GLFWmonitor* monitor = fullscreen ? glfwGetPrimaryMonitor() : NULL;
	GLFWmonitor* monitor = NULL;
	// Create window with graphics context
	GLFWwindow* window = glfwCreateWindow(default_window_width, default_window_height, "Go", monitor, NULL);
	if (window == NULL)
		return { 1, nullptr };
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1); // Enable vsync

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	//ImGui::StyleColorsLight();

	// Setup Platform/Renderer backends
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);

	return { 0, window };
}

template<typename T>
void erase_indexs(std::vector<T>& vector, const std::span<size_t> idxs)
{
	auto vector_base = vector.begin();
	typename std::vector<T>::size_type removed = 0;

	for (auto iter = idxs.begin();
		iter < idxs.end();
		iter++, removed++)
	{
		typename std::vector<T>::size_type next = ((iter + 1) == idxs.end()
			? vector.size()
			: *(iter + 1));

		std::move(vector_base + *iter + 1,
			vector_base + next,
			vector_base + *iter - removed);
	}

	vector.erase(vector.begin() + vector.size() - idxs.size(), vector.end());
}

// large full size boards are 19x19
// beginer boards are 9x9
//template<uint32_t board_height, uint32_t board_width>
struct go_ctx {
	static constexpr size_t empty = 0ull;

	using tile_data = uint8_t;
	using team_data = tile_data;
	//we can limit the required size for this given the board_width and height
	using marker_idx = uint64_t;

	static constexpr size_t max_teams = (1 << sizeof(tile_data) * 8);

	//std::array<tile_data, board_height* board_width> board = {};
	uint32_t board_height = {};
	uint32_t board_width = {};

	std::vector<tile_data> board = {};
	std::array<uint32_t, max_teams> team_scores = {};

	using unit = std::vector<marker_idx>;
	using units = std::vector<unit>;
	std::array<units, max_teams> team_units = {};

	//
	size_t teams_at_play = {}; // must be 2 or higher

	//
	struct ko_result {
		marker_idx marker_id = {};
		uint32_t turns = {};
		//uint32_t team = {};
	};

	ko_result ko = {};

	// we'll use this for any complicated shinanigans
	std::vector<size_t> temporary_data = {};

	void remove_marker_unchecked(uint32_t y, uint32_t x) {
		board[y * board_width + x] = 0ull;
	}

	size_t capture(uint32_t y, uint32_t x, team_data team) {
		if (y > board_height || x > board_width)
			return ~size_t{ 0 };
		if (board[y * board_width + x] == empty)
			return ~size_t{ 0 };

		team_scores[team] += 1;
		remove_marker_unchecked(y, x);
		return (y * board_width) + x;
	}

	size_t capture_unit_unchecked(size_t unit_idx, team_data target_team, team_data for_team) {
		units& target_units = team_units[target_team];
		unit& target_unit = target_units[unit_idx];
		size_t last_capture = ~size_t{ 0 };

		for (size_t i = 0; i < target_unit.size(); i++) {
			size_t marker_idx = target_unit[i];
			last_capture = capture(marker_idx / board_width, marker_idx % board_width, for_team);
		}
		target_units.erase(target_units.begin() + unit_idx);
		return last_capture;
	}

	struct capture_result {
		uint32_t team = {};
		uint32_t unit = {};
	};

	uint8_t spaces_occupied_by_team_around(uint32_t y, uint32_t x, team_data t) noexcept {
		uint8_t neighbor_up_teammate = (y + 1 < board_height
			&& board[(y + 1) * board_width + x] == t);

		uint8_t neighbor_down_teammate = (y - 1 < board_height &&
			board[(y - 1) * board_width + x] == t) << 1;

		uint8_t neighbor_right_teammate = (y + 1 < board_width &&
			board[(y)*board_width + (x + 1)] == t) << 2;

		uint8_t neighbor_left_teammate = (y - 1 < board_width &&
			board[(y)*board_width + (x - 1)] == t) << 3;

		return neighbor_up_teammate | neighbor_down_teammate | neighbor_right_teammate | neighbor_left_teammate;
	}

	uint8_t spaces_free_around(uint32_t y, uint32_t x) noexcept {
		return spaces_occupied_by_team_around(y, x, empty);
	}

	capture_result check_for_captures(uint32_t y, uint32_t x, team_data team) {
		capture_result result = {};

		for (size_t opponent_team = 1; opponent_team < (teams_at_play + 1); opponent_team++) {
			if (opponent_team == team)
				continue;

			units& tunits = team_units[opponent_team];
			size_t liberties = 0ull;
			for (size_t current_unit = 0; current_unit < tunits.size(); current_unit++) {
				unit& tunit = tunits[current_unit];

				liberties = 0ull;
				size_t space_free = 0ull;
				for (const marker_idx& idx : tunit) {
					size_t marker_y = idx / board_width;
					size_t marker_x = idx % board_width;

					uint8_t neighbor_up_empty = (marker_y + 1 < board_height
						&& board[(marker_y + 1) * board_width + marker_x] == empty);

					uint8_t neighbor_down_empty = (marker_y - 1 < board_height &&
						board[(marker_y - 1) * board_width + marker_x] == empty);

					uint8_t neighbor_right_empty = (marker_x + 1 < board_width &&
						board[(marker_y)*board_width + (marker_x + 1)] == empty);

					uint8_t neighbor_left_empty = (marker_x - 1 < board_width &&
						board[(marker_y)*board_width + (marker_x - 1)] == empty);

					liberties += (neighbor_up_empty && ((marker_y + 1) * board_width + marker_x) != space_free) + (neighbor_down_empty && ((marker_y - 1) * board_width + marker_x) != space_free)
						+ (neighbor_right_empty && ((marker_y)*board_width + (marker_x + 1)) != space_free) + (neighbor_left_empty && ((marker_y)*board_width + (marker_x - 1)) != space_free);

					if (neighbor_up_empty && ((marker_y + 1) * board_width + marker_x) != space_free)
						space_free = ((marker_y + 1) * board_width + marker_x);

					if (neighbor_down_empty && ((marker_y - 1) * board_width + marker_x) != space_free)
						space_free = ((marker_y - 1) * board_width + marker_x);

					if (neighbor_right_empty && ((marker_y)*board_width + (marker_x + 1)) != space_free)
						space_free = ((marker_y)*board_width + (marker_x + 1));

					if (neighbor_left_empty && ((marker_y)*board_width + (marker_x - 1)) != space_free)
						space_free = ((marker_y)*board_width + (marker_x - 1));
				}

				if (liberties == 1 && space_free == (y * board_width + x)) {
					// ?
					result.team = opponent_team;
					result.unit = current_unit;
					return result;
				}
			}
		}

		return result;
	}

	capture_result check_for_self_capture(uint32_t y, uint32_t x, team_data team, size_t start_unit = 0, size_t captured_unit = 0) {
		capture_result result = {};

		units& tunits = team_units[team];
		size_t liberties = 0ull;
		for (size_t current_unit = start_unit; current_unit < tunits.size(); current_unit++) {
			unit& tunit = tunits[current_unit];

			size_t space_free = ~size_t{ 0 };
			for (const marker_idx& idx : tunit) {
				size_t marker_y = idx / board_width;
				size_t marker_x = idx % board_width;

				uint8_t neighbor_up_empty = (marker_y + 1 < board_height
					&& board[(marker_y + 1) * board_width + marker_x] == empty);

				uint8_t neighbor_down_empty = (marker_y - 1 < board_height &&
					board[(marker_y - 1) * board_width + marker_x] == empty);

				uint8_t neighbor_right_empty = (marker_x + 1 < board_width &&
					board[(marker_y)*board_width + (marker_x + 1)] == empty);

				uint8_t neighbor_left_empty = (marker_x - 1 < board_width &&
					board[(marker_y)*board_width + (marker_x - 1)] == empty);

				liberties += (neighbor_up_empty && ((marker_y + 1) * board_width + marker_x) != space_free) + (neighbor_down_empty && ((marker_y - 1) * board_width + marker_x) != space_free)
					+ (neighbor_right_empty && ((marker_y)*board_width + (marker_x + 1)) != space_free) + (neighbor_left_empty && ((marker_y)*board_width + (marker_x - 1)) != space_free);

				if (neighbor_up_empty && ((marker_y + 1) * board_width + marker_x) != space_free)
					space_free = ((marker_y + 1) * board_width + marker_x);

				if (neighbor_down_empty && ((marker_y - 1) * board_width + marker_x) != space_free)
					space_free = ((marker_y - 1) * board_width + marker_x);

				if (neighbor_right_empty && ((marker_y)*board_width + (marker_x + 1)) != space_free)
					space_free = ((marker_y)*board_width + (marker_x + 1));

				if (neighbor_left_empty && ((marker_y)*board_width + (marker_x - 1)) != space_free)
					space_free = ((marker_y)*board_width + (marker_x - 1));
			}

			if (captured_unit == 0 && liberties == 1 && space_free == (y * board_width + x)) {
				// ?
				result.team = team;
				result.unit = current_unit;
				return result;
			}

			if (captured_unit != 0 && liberties == 1 && space_free == (y * board_width + x)) {
				captured_unit--;
			}
		}

		return result;
	}

	inline bool same_marker(uint32_t y0, uint32_t x0, uint32_t y1, uint32_t x1) noexcept {
		return y0 == y1 && x0 == x1;
	}

	size_t units_around_for_team(uint32_t y, uint32_t x, team_data team) {
		uint32_t marker_id = y * board_width + x;

		units& tunits = team_units[team];

		size_t count = 0ull;

		for (size_t current_unit = 0; current_unit < tunits.size(); current_unit++) {
			unit& tunit = tunits[current_unit];
			for (const marker_idx& idx : tunit) {
				size_t marker_y = idx / board_width;
				size_t marker_x = idx % board_width;

				bool marker_belongs_to_unit = (marker_y + 1 < board_height
					&& same_marker(marker_y + 1, marker_x, y, x)) || (marker_y - 1 < board_height &&
						same_marker(marker_y - 1, marker_x, y, x)) ||
					(marker_x + 1 < board_width &&
						same_marker(marker_y, marker_x + 1, y, x)) ||
					(marker_x - 1 < board_width &&
						same_marker(marker_y, marker_x - 1, y, x));

				if (marker_belongs_to_unit) {
					count++;
					break;
				}
			}
		}

		return count;
	}

	void form_unit_for_team(uint32_t y, uint32_t x, team_data team,
		std::vector<size_t>& expanded_units_tmp_stack) {
		uint32_t marker_id = y * board_width + x;

		units& tunits = team_units[team];

		size_t tmp_idx = expanded_units_tmp_stack.size();

		for (size_t current_unit = 0; current_unit < tunits.size(); current_unit++) {
			unit& tunit = tunits[current_unit];
			for (const marker_idx& idx : tunit) {
				size_t marker_y = idx / board_width;
				size_t marker_x = idx % board_width;

				bool marker_belongs_to_unit = (marker_y + 1 < board_height
					&& same_marker(marker_y + 1, marker_x, y, x)) || (marker_y - 1 < board_height &&
						same_marker(marker_y - 1, marker_x, y, x)) ||
					(marker_x + 1 < board_width &&
						same_marker(marker_y, marker_x + 1, y, x)) ||
					(marker_x - 1 < board_width &&
						same_marker(marker_y, marker_x - 1, y, x));

				if (marker_belongs_to_unit) {
					expanded_units_tmp_stack.emplace_back(current_unit);
					break;
				}
			}
		}

		if (tmp_idx < expanded_units_tmp_stack.size()) {
			tunits[expanded_units_tmp_stack[tmp_idx]].emplace_back(marker_id);

			// merge all other units into the first
			for (size_t i = tmp_idx + 1; i < expanded_units_tmp_stack.size(); i++) {
				unit& main_unit = tunits[expanded_units_tmp_stack[tmp_idx]];
				unit& merge_unit = tunits[expanded_units_tmp_stack[i]];
				for (size_t mark = 0; mark < merge_unit.size(); mark++) {
					main_unit.emplace_back(merge_unit[mark]);
				}
			}

			// remove all the other units
			size_t removed = 0ull;
			if (tmp_idx + 1 < expanded_units_tmp_stack.size()) {
				erase_indexs(tunits, std::span{ expanded_units_tmp_stack.data() + (tmp_idx + 1),
					expanded_units_tmp_stack.size() - (tmp_idx + 1)
					});
			}
		}
		else {
			// add a new unit
			unit& tunit = tunits.emplace_back();
			tunit.emplace_back(marker_id);
		}

		// discard temporary data
		expanded_units_tmp_stack.erase(expanded_units_tmp_stack.begin() + tmp_idx, expanded_units_tmp_stack.end());
	}

	bool place_marker(uint32_t y, uint32_t x, team_data team) {
		if (y > board_height || x > board_width)
			return false;
		if (board[y * board_width + x] != empty)
			return false;
		if (ko.marker_id == (y * board_width) + x)
			return false;

		capture_result r = check_for_captures(y, x, team);
		//capture_result r_self_2 = check_for_self_capture(y, x, team);
		uint8_t free_intersections = spaces_free_around(y, x);
		uint8_t friendly_intersections = spaces_occupied_by_team_around(y, x, team);

		// there's a space next to this available, a legal move (simple)
		if (r.team == empty && free_intersections != 0) { //std::popcount(free_intersections) >= 1
			board[y * board_width + x] = team;
			form_unit_for_team(y, x, team, temporary_data);
			ko.marker_id = ~size_t{ 0ull };
			return true;
		}

		// if we can capture something (we'll definitely get a liberty so always allowed)
		if (r.team != empty) {
			size_t last_capture = ~size_t{ 0 };
			size_t previous_score = team_scores[team];
			do {
				last_capture = capture_unit_unchecked(r.unit, r.team, team);
			} while ((r = check_for_captures(y, x, team)), r.team != empty);

			// ko as described reads as though we need to keep a full history of
			// all moves of the game*, which would require an insane amount of space
			// I don't know if this properly implements "ko"
			// but it should prevent infinitely repeating moves
			if ((team_scores[team] - previous_score) == 1)
				ko.marker_id = last_capture;
			else
				ko.marker_id = ~size_t{ 0ull };

			board[y * board_width + x] = team;
			form_unit_for_team(y, x, team, temporary_data);

			return true;
		}

		capture_result r_self_0 = check_for_self_capture(y, x, team);
		capture_result r_self_1 = check_for_self_capture(y, x, team, r_self_0.unit + 1);
		capture_result r_self_2 = check_for_self_capture(y, x, team, r_self_1.unit + 1);
		capture_result r_self_3 = check_for_self_capture(y, x, team, r_self_2.unit + 1);

		size_t unit_count = units_around_for_team(y, x, team);

		// no space available and we didn't capture*

		// we must not make a move that "self" captures
		// eg: we can't make a move that removes the last liberty
		// from one of our units which at risk of being captured
		if ((unit_count == 4 && r_self_3.team == team) ||
			(unit_count == 3 && r_self_2.team == team) ||
			(unit_count == 2 && r_self_1.team == team) ||
			(unit_count == 1 && r_self_0.team == team)) { //r.team == empty && 
			return false;
		}

		// if we're not at risk of self capture but the unit is surrounded
		// we're still a legal move?
		if (friendly_intersections > 0) {
			board[y * board_width + x] = team;
			form_unit_for_team(y, x, team, temporary_data);
			ko.marker_id = ~size_t{ 0ull };
			return true;
		}

		return false;
	}

	void reserve_board_data(uint32_t new_board_height, uint32_t new_board_width, team_data team_count = 2) {
		size_t maximum_unit_count = (board_height * board_width / team_count) + 1; //+1 to avoid the lopping off of a tile for odd teams
		board.reserve(new_board_height * new_board_width);
		for (size_t i = 0; i < team_units.size(); i++) {
			team_units[i].reserve(maximum_unit_count);
		}
	}

	void initialize_board(uint32_t new_board_height = 19, uint32_t new_board_width = 19, team_data team_count = 2) {
		teams_at_play = team_count;
		board_height = new_board_height;
		board_width = new_board_width;

		board.clear();
		reserve_board_data(new_board_height, new_board_width, team_count);

		for (size_t i = 0; i < (new_board_height * new_board_width); i++) {
			board.emplace_back(); //(uint8_t)empty
		}
		for (size_t i = 0; i < team_scores.size(); i++) {
			team_scores[i] = 0;
		}
	}
};

struct go_board_bounds {
	uint32_t y0;
	uint32_t x0;
	uint32_t y1;
	uint32_t x1;
};

struct go_conditional_move {
	go_board_bounds bounds = {};
	uint32_t y;
	uint32_t x;
	uint32_t team;
};

struct go_match {
	uint64_t game_id = {}; //unique id
	uint64_t host = {}; //ipv4/6 address
	uint32_t port = {}; //

	go_ctx go_game = {}; //
	go_ctx::team_data team_turn = 1; //

	go_ctx::team_data next_to_play_after(go_ctx::team_data t) {
		t += 1;
		t %= (go_game.teams_at_play + 1);
		t += (t == go_ctx::empty);

		return t;
	}


	std::vector<go_conditional_move> conditional_move = {}; //
};

struct go_match_settings {
	std::vector<ImU32> team_color_palette = {};
	ImU32 wood_color = ImU32{ 0xff67afdb };
	ImU32 line_color = ImU32{ 0xff030303 };
};

struct go_board_interaction {
	float width = {};
	float height = {};

	uint32_t interest_x = {};
	uint32_t interest_y = {};

	bool left_clicked = false;
	bool right_clicked = false;
	bool hovered_tile = false;
	bool hovered_board = false;
};

go_board_interaction draw_go_board(go_ctx& go_game, const std::vector<ImU32>& team_color_palette, ImU32 wood_color, ImU32 line_color) {
	//ImGui::Begin("##go", nullptr, 0);
	//ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar
	//| ImGuiWindowFlags_::ImGuiWindowFlags_NoMove
	//|ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse
	go_board_interaction result = {};
	result.interest_x = ~0;
	result.interest_y = ~0;

	ImVec2 cursor_position = ImGui::GetCursorScreenPos();
	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	float window_width = ImGui::CalcItemWidth();
	float window_height = ImGui::GetWindowHeight();

	float full_width = std::max(window_width, (float)20 * (go_game.board_width));
	float full_height = std::max(window_height, (float)20 * (go_game.board_height));

	float tile_xdim = full_width / (go_game.board_width + 1);
	float tile_ydim = full_height / (go_game.board_height + 1);

	int tile_dim = std::min(tile_xdim, tile_ydim);

	float line_width = tile_dim / 20.0f;

	// give 1/2 tile spacing all around
	result.width = (float)tile_dim * (go_game.board_width + 1);
	result.height = (float)tile_dim * (go_game.board_height + 1);

	ImVec2 mouse = ImGui::GetMousePos();
	bool ok_mouse = ImGui::IsMousePosValid(&mouse);

	//ImVec2 grid_top_left = { (float)0 * tile_dim, (float)0 * tile_dim };
	//ImVec2 grid_btm_right = { (float)(go_game.board_width + 1) * tile_dim, (float)(go_game.board_height + 1) * tile_dim };

	//bool mouse_in_grid = ImGui::IsMouseHoveringRect(grid_top_left, grid_btm_right);

	bool left_clicked = ok_mouse && ImGui::IsMouseClicked(ImGuiMouseButton_::ImGuiMouseButton_Left);
	bool right_clicked = ok_mouse && ImGui::IsMouseClicked(ImGuiMouseButton_::ImGuiMouseButton_Right);

	//ImDrawList* draw_list = ImGui::GetWindowDrawList();

	// background
	{
		ImVec2 background_topleft = { (float)tile_dim / 2
			+ cursor_position.x, (float)tile_dim / 2 + cursor_position.y };

		ImVec2 background_bottomright = {
				(float)tile_dim * go_game.board_width + (tile_dim / 2)
				+ cursor_position.x,
			(float)tile_dim * go_game.board_height + (tile_dim / 2) +
			cursor_position.y };

		draw_list->AddRectFilled(background_topleft,
			background_bottomright,
			wood_color);
		result.hovered_board = ImGui::IsMouseHoveringRect(background_topleft, background_bottomright);
	}

	// draw lines
	for (size_t i = 1; i < (go_game.board_width + 1); i++) {
		draw_list->AddLine(ImVec2{ (float)i * tile_dim + cursor_position.x, (float)tile_dim + cursor_position.y },
			ImVec2{ (float)i * tile_dim + cursor_position.x, (float)go_game.board_height * tile_dim + cursor_position.y }, line_color, line_width);
	}

	for (size_t i = 1; i < (go_game.board_height + 1); i++) {
		draw_list->AddLine(
			ImVec2{ (float)tile_dim + cursor_position.x, (float)i * tile_dim + cursor_position.y },
			ImVec2{ (float)go_game.board_width * tile_dim + cursor_position.x, (float)i * tile_dim + cursor_position.y }, line_color, line_width);
	}

	// fixup grid corners
	draw_list->AddRect(ImVec2{ (float)tile_dim + cursor_position.x, (float)tile_dim + cursor_position.y },
		ImVec2{ (float)tile_dim * go_game.board_width + cursor_position.x, (float)tile_dim * go_game.board_height + cursor_position.y },
		line_color, 0.0f, 0, line_width * 1.1f
	);

	//19x19 (19 / 2) -> 9, (9 / 2) -> 4
	//0 1 2 3 ||4|| 5 6 7 8 |9| 10 11 12 13 ||14|| 15 16 17 18

	// star points
	uint32_t mid_star_y = go_game.board_height / 2;
	uint32_t low_star_y = mid_star_y / 2;
	uint32_t high_star_y = (go_game.board_height - 1) - low_star_y;

	uint32_t mid_star_x = go_game.board_width / 2;
	uint32_t low_star_x = mid_star_x / 2;
	uint32_t high_star_x = (go_game.board_width - 1) - low_star_x;

	{
		draw_list->AddCircleFilled(
			ImVec2{ (float)tile_dim * (low_star_y + 1) + cursor_position.x, (float)tile_dim * (low_star_x + 1) + cursor_position.y }, (line_width * 2.0f),
			line_color);
		/*
		draw_list->AddCircleFilled(
			ImVec2{ (float)tile_dim * (low_star_y + 1), (float)tile_dim * (mid_star_x + 1) }, (line_width * 2.5f),
			black);
			*/
		draw_list->AddCircleFilled(
			ImVec2{ (float)tile_dim * (low_star_y + 1) + cursor_position.x, (float)tile_dim * (high_star_x + 1) + cursor_position.y }, (line_width * 2.0f),
			line_color);

		/*
		draw_list->AddCircleFilled(
			ImVec2{ (float)tile_dim * (mid_star_y + 1), (float)tile_dim * (low_star_x + 1) }, (line_width * 2.5f),
			black);
			*/
		draw_list->AddCircleFilled(
			ImVec2{ (float)tile_dim * (mid_star_y + 1) + cursor_position.x, (float)tile_dim * (mid_star_x + 1) + cursor_position.y }, (line_width * 2.0f),
			line_color);
		/*
		draw_list->AddCircleFilled(
			ImVec2{ (float)tile_dim * (mid_star_y + 1), (float)tile_dim * (high_star_x + 1) }, (line_width * 2.5f),
			black);
			*/
		draw_list->AddCircleFilled(
			ImVec2{ (float)tile_dim * (high_star_y + 1) + cursor_position.x, (float)tile_dim * (low_star_x + 1) + cursor_position.y }, (line_width * 2.0f),
			line_color);
		/*
		draw_list->AddCircleFilled(
			ImVec2{ (float)tile_dim * (high_star_y + 1), (float)tile_dim * (mid_star_x + 1) }, (line_width * 2.5f),
			black);
			*/
		draw_list->AddCircleFilled(
			ImVec2{ (float)tile_dim * (high_star_y + 1) + cursor_position.x, (float)tile_dim * (high_star_x + 1) + cursor_position.y }, (line_width * 2.0f),
			line_color);
	}

	bool hovering_over_intersection = false;

	for (size_t y = 0; y < go_game.board_height; y++) {
		for (size_t x = 0; x < go_game.board_width; x++) {
			ImVec2 top_left = { (float)tile_dim * (x + 1) - (tile_dim / 4) + cursor_position.x, (float)tile_dim * (y + 1) - (tile_dim / 4) + cursor_position.y };
			ImVec2 bottom_right = { (float)tile_dim * (x + 1) + (tile_dim / 4) + cursor_position.x, (float)tile_dim * (y + 1) + (tile_dim / 4) + cursor_position.y };

			bool is_hovering = ImGui::IsMouseHoveringRect(top_left, bottom_right);
			hovering_over_intersection = hovering_over_intersection || is_hovering;

			if (is_hovering) {
				result.hovered_tile = true;
				result.interest_x = x;
				result.interest_y = y;
			}

			bool placed_marker = false; //go_game.board[y * go_game.board_width + x] != go_ctx::empty;

			if (is_hovering && left_clicked) {
				result.left_clicked = true;
				result.interest_x = x;
				result.interest_y = y;
			}

			if (is_hovering && right_clicked) {
				result.right_clicked = true;
				result.interest_x = x;
				result.interest_y = y;
			}
			//ImGui::InvisibleButton("##intersection");

#if 0
			try {
				if (!placed_marker && is_hovering && left_clicked) {
					placed_marker = go_game.place_marker(y, x, 1ull);
					if (placed_marker) {
						bool accepted_move = client.call("perform_move", (uint32_t)y, (uint32_t)x, (uint8_t)1ull).as<bool>();
					}
				}
				else if (!placed_marker && is_hovering && right_clicked) {
					placed_marker = go_game.place_marker(y, x, 2ull);
					if (placed_marker) {
						//client.call("perform_move", (uint32_t)y, (uint32_t)x, (uint8_t)2ull);
						bool accepted_move = client.call("perform_move", (uint32_t)y, (uint32_t)x, (uint8_t)2ull).as<bool>();
					}
				}
			}
			catch (rpc::rpc_error& e) {
				std::cout << std::endl << e.what() << std::endl;
				std::cout << "in function '" << e.get_function_name() << "': ";

				using err_t = std::tuple<int, std::string>;
				auto err = e.get_error().as<err_t>();
				std::cout << "[error " << std::get<0>(err) << "]: " << std::get<1>(err)
					<< std::endl;
				return 1;
			}
#endif

			if (go_game.board[y * go_game.board_width + x] != go_ctx::empty) {
				draw_list->AddCircleFilled(
					ImVec2{ (float)tile_dim * (x + 1) + cursor_position.x, (float)tile_dim * (y + 1) + cursor_position.y },
					((tile_dim * 3) / 8),
					team_color_palette[go_game.board[y * go_game.board_width + x] % team_color_palette.size()]);
			}
			// if placed marker -> alternate teams
		}
	}


	//ImGuiWindowFlags overlay_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	//ImGui::SetNextWindowPos(cursor_position, ImGuiCond_Always);
	if (hovering_over_intersection) {
		//if (ImGui::Begin("##info", nullptr, overlay_flags))
		//{
		if (result.interest_x < go_game.board_width && result.interest_y < go_game.board_height) {
			//char tmp_buf[64] = {};
			stack_vector::stack_vector<char, 64> tmp_buf = {};
			fmt::format_to(std::back_inserter(tmp_buf), "Intersection: ({},{})", result.interest_x, result.interest_y);
			draw_list->AddText(cursor_position, ImU32{0xffffffff}, tmp_buf.data(), tmp_buf.data()+tmp_buf.size());
			//draw_list->AddText()
			//ImGui::Text("Intersection: (%d, %d)", hover_x, hover_y);
		}
		else {
			std::string_view v = "Intersection: (?, ?)";
			draw_list->AddText(cursor_position, ImU32{ 0xffffffff }, v.data(), v.data()+v.size());
			//ImGui::Text("Intersection: (?, ?)");
		}
		//	ImGui::End();
		//}
	}

	//ImGui::End();
	return result;
}

go_board_interaction draw_go_match(go_match& match, go_match_settings& settings) {
	return draw_go_board(match.go_game, settings.team_color_palette, settings.wood_color, settings.line_color);
}

int main(int argc, char** argv)
{
	uint32_t window_width = 1920;
	uint32_t window_height = 1080;
	glfw3_setup_t r = glfw3_setup(window_width, window_height);

	bool show_demo_window = true;

	std::vector<go_match> matches = {}; // list of games/opponents
	std::vector<go_match_settings> settings = {}; // local to player

	//TODO: load previous matches and settings

	matches.emplace_back();
	settings.emplace_back();
	std::array<ImU32, 9> default_team_color_palette = {
		ImU32{0xffFAF9F8},
		ImU32{0xff292521},

		ImU32{0xffEFECE9},
		ImU32{0xff403A34},

		ImU32{0xffE6E2DE},
		ImU32{0xff575049},

		ImU32{0xffDAD4CE},
		ImU32{0xff7D756C},

		ImU32{0xffBDB5AD}
	};

	settings[0].team_color_palette.reserve(default_team_color_palette.size());
	for (size_t i = 0; i < default_team_color_palette.size(); i++) {
		settings[0].team_color_palette.emplace_back(default_team_color_palette[i]);
	}
	matches[0].go_game.initialize_board(); //defaults to full sized board

	//go_ctx go_game = {};
	go_ctx::team_data team_to_move = 1ull;
	/*
	uint32_t x_tiles = 9;
	uint32_t y_tiles = 9;
	*/
	//go_game.initialize_board();

	/*
	int server_port = 8080;
	std::jthread server_thread{ [&server_port, &go_game, &team_to_move]() {
		rpc::server srv(server_port);

		srv.bind("peform_move", [&go_game, &team_to_move](uint32_t y, uint32_t x, go_ctx::team_data t) {
			if (t != team_to_move)
				return false;
			bool can_move = go_game.place_marker(y, x, t);
			if (can_move) {
				team_to_move += 1;
				team_to_move %= (go_game.teams_at_play + 1);
				team_to_move += (t == 0); //skip empty team
			}
			return can_move;
		});

		srv.run();
	}};

	*/
	/*
	using namespace zpp::bits::literals;
	// Server and client together:
	using rpc = zpp::bits::rpc<
		zpp::bits::bind<foo, "foo"_sha256_int>,
		zpp::bits::bind<bar, "bar"_sha256_int>
	>;
	auto [client, server] = rpc::client_server(in, out);
	*/
	int server_port = rpc::constants::DEFAULT_PORT;
	rpc::server srv(server_port);
#if 0
	srv.bind("perform_move", [&go_game, &team_to_move](uint32_t y, uint32_t x, go_ctx::team_data t) {
		if (t != team_to_move)
			return false;
		bool can_move = go_game.place_marker(y, x, t);
		if (can_move) {
			team_to_move += 1;
			team_to_move %= (go_game.teams_at_play + 1);
			team_to_move += (team_to_move == 0); //skip empty team
		}
		return can_move;
		});

	bool querying_new_game = false;
	bool accepts_new_game = false;
	bool rejects_new_game = false;
	srv.bind("query_new_game", [&go_game, &accepts_new_game, &rejects_new_game, &querying_new_game](uint32_t y, uint32_t x, go_ctx::team_data players) {
		if (players < 2)
			return false;

		using namespace std::chrono_literals;
		querying_new_game = true;
		accepts_new_game = false;
		rejects_new_game = false;

		while (!accepts_new_game || !rejects_new_game) {
			std::this_thread::sleep_for(16ms);
		}

		if (accepts_new_game) {

		}
		else {

}
		});

	srv.async_run();
	rpc::client client("127.0.0.1", server_port);
#endif
	// wood dbaf67
	ImU32 gray = ImU32{ 0xffc1c0c1 };
	ImU32 dark_gray = ImU32{ 0xff4d4d4d };
	ImU32 red = ImU32{ 0xff2828f2 };

	uint32_t study_board_width = 19;
	uint32_t study_board_height = 19;

	go_match study = {};
	go_match_settings study_settings = {};
	study.go_game.initialize_board(study_board_width, study_board_height);

	for (size_t i = 0; i < default_team_color_palette.size(); i++) {
		study_settings.team_color_palette.emplace_back(default_team_color_palette[i]);
	}

	while (!glfwWindowShouldClose(r.window))
	{
		// Poll and handle events (inputs, window resize, etc.)
		// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
		// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
		// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
		// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
		glfwPollEvents();

		// Start the Dear ImGui frame
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		//ImGui::ShowDemoWindow(&show_demo_window);
		/*
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImVec2 work_pos = viewport->WorkPos; // Use work area to avoid menu-bar/task-bar, if any!
		ImVec2 work_size = viewport->WorkSize;

		ImGui::GetMainViewport()->GetCenter();

		ImGui::Indent();
		ImGui::Unindent();
		*/
		/*
		* Drawing a custom gradient (rectangle)
			ImGui::Text("Gradients");
			ImVec2 gradient_size = ImVec2(ImGui::CalcItemWidth(), ImGui::GetFrameHeight()); //get width and height
			{
				ImVec2 p0 = ImGui::GetCursorScreenPos(); // get the position ImGui would draw
				ImVec2 p1 = ImVec2(p0.x + gradient_size.x, p0.y + gradient_size.y);
				ImU32 col_a = ImGui::GetColorU32(IM_COL32(0, 0, 0, 255));
				ImU32 col_b = ImGui::GetColorU32(IM_COL32(255, 255, 255, 255));
				draw_list->AddRectFilledMultiColor(p0, p1, col_a, col_b, col_b, col_a); // draw
				ImGui::InvisibleButton("##gradient1", gradient_size); // place an invisible item to bump where imgui would draw next
			}
		*/
		//ImGuiTabBarFlags_FittingPolicyMask_
		if (ImGui::BeginTabBar("Tabs")) {
			//ImGui::Begin("##go", nullptr, 0);
			if (ImGui::BeginTabItem("Study", nullptr, ImGuiTabItemFlags_None)) {
				go_board_interaction interaction = draw_go_match(study, study_settings);
				if (interaction.left_clicked) {
					study.go_game.place_marker(interaction.interest_y, interaction.interest_x, study.team_turn);
					study.team_turn = study.next_to_play_after(study.team_turn);
				} else if (interaction.right_clicked) {
					study.go_game.remove_marker_unchecked(interaction.interest_y, interaction.interest_x);
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("New Game", nullptr, ImGuiTabItemFlags_None))
			{
				ImGui::Text("Host New Game Menu");
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Join Game", nullptr, ImGuiTabItemFlags_None)) {
				ImGui::Text("Join Game Menu");
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Games List", nullptr, ImGuiTabItemFlags_None))
			{
				//ImGui::Text("Games List");

				for (size_t i = 0; i < matches.size() && i < settings.size(); i++) {
					go_board_interaction interaction = draw_go_match(matches[i], settings[i]);
					ImGui::InvisibleButton("##MatchOverview", ImVec2{ interaction.width,interaction.height });
				}

				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		//int width;
		//int height;
		//glfwGetFramebufferSize(r.window, &width, &height);
		//ImGui::SetNextWindowSize(ImVec2(width, height)); // ensures ImGui fits the GLFW window

		//int tile_xdim = width / (go_game.board_width + 1);
		//int tile_ydim = height / (go_game.board_height + 1);

		//int tile_dim = std::min(tile_xdim, tile_ydim);
		//ImGui::SetNextWindowPos(ImVec2(0, 0));

		ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
		// Rendering
		ImGui::Render();
		int display_w, display_h;
		glfwGetFramebufferSize(r.window, &display_w, &display_h);
		glViewport(0, 0, display_w, display_h);
		glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		glfwSwapBuffers(r.window);
	}

	// Cleanup
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(r.window);
	glfwTerminate();

	return r.err_code;
}

/*
// boilerplate ~for windows build~
int WinMain(int argc, char** argv) {
	return main(argc, argv);
}
*/