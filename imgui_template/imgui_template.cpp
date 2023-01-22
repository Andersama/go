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

	// we'll use this for any complicated shinanigans
	std::vector<size_t> temporary_data = {};

	void remove_marker_unchecked(uint32_t y, uint32_t x) {
		board[y * board_width + x] = 0ull;
	}

	void capture(uint32_t y, uint32_t x, team_data team) {
		if (y > board_height || x > board_width)
			return;
		if (board[y * board_width + x] == empty)
			return;

		team_scores[team] += 1;
		remove_marker_unchecked(y, x);
	}

	void capture_unit_unchecked(size_t unit_idx, team_data target_team, team_data for_team) {
		units& target_units = team_units[target_team];
		unit& target_unit = target_units[unit_idx];
		for (size_t i = 0; i < target_unit.size(); i++) {
			size_t marker_idx = target_unit[i];
			capture(marker_idx / board_width, marker_idx % board_width, for_team);
		}
		target_units.erase(target_units.begin() + unit_idx);
	}

	struct capture_result {
		uint32_t team = {};
		uint32_t unit = {};
	};

	uint8_t spaces_occupied_by_team_around(uint32_t y, uint32_t x, team_data t) noexcept {
		uint8_t neighbor_up_teammate = (y + 1 < board_height
			&& board[(y + 1) * board_width + y] == t);

		uint8_t neighbor_down_teammate = (y - 1 < board_height &&
			board[(y - 1) * board_width + y] == t) << 1;

		uint8_t neighbor_right_teammate = (y + 1 < board_width &&
			board[(y)*board_width + (y + 1)] == t) << 2;

		uint8_t neighbor_left_teammate = (y - 1 < board_width &&
			board[(y)*board_width + (y - 1)] == t) << 3;

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

					liberties += neighbor_up_empty + neighbor_down_empty + neighbor_right_empty + neighbor_left_empty;

					if (neighbor_up_empty)
						space_free = ((marker_y + 1) * board_width + marker_x);
					
					if (neighbor_down_empty)
						space_free = ((marker_y - 1) * board_width + marker_x);
					
					if (neighbor_right_empty)
						space_free = ((marker_y) * board_width + (marker_x + 1));

					if (neighbor_left_empty)
						space_free = ((marker_y) * board_width + (marker_x-1));
				}

				if (liberties == 1 && space_free == (y*board_width + x)) {
					// ?
					result.team = opponent_team;
					result.unit = current_unit;
					return result;
				}
			}
		}

		return result;
	}

	capture_result check_for_self_capture(uint32_t y, uint32_t x, team_data team) {
		capture_result result = {};

		units& tunits = team_units[team];
		size_t liberties = 0ull;
		for (size_t current_unit = 0; current_unit < tunits.size(); current_unit++) {
			unit& tunit = tunits[current_unit];

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

				liberties += neighbor_up_empty + neighbor_down_empty + neighbor_right_empty + neighbor_left_empty;

				if (neighbor_up_empty)
					space_free = ((marker_y + 1) * board_width + marker_x);

				if (neighbor_down_empty)
					space_free = ((marker_y - 1) * board_width + marker_x);

				if (neighbor_right_empty)
					space_free = ((marker_y)*board_width + (marker_x + 1));

				if (neighbor_left_empty)
					space_free = ((marker_y)*board_width + (marker_x - 1));
			}

			if (liberties == 1 && space_free == (y * board_width + x)) {
				// ?
				result.team = team;
				result.unit = current_unit;
				return result;
			}
		}

		return result;
	}

	inline bool same_marker(uint32_t y0, uint32_t x0, uint32_t y1, uint32_t x1) noexcept {
		return y0 == y1 && x0 == x1;
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

				if (marker_belongs_to_unit)
					expanded_units_tmp_stack.emplace_back(current_unit);
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
		} else {
			// add a new unit
			unit &tunit = tunits.emplace_back();
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

		capture_result r = check_for_captures(y, x, team);
		capture_result r_self = check_for_self_capture(y, x, team);

		if (r.team == empty && r_self.team == empty) {
			board[y * board_width + x] = team;
			form_unit_for_team(y, x, team, temporary_data);
			return true;
		}
		else if (r.team == empty && r_self.team == team) {
			// self capture not allowed?
			return false;
		}
		else if (r.team != empty) {
			do {
				capture_unit_unchecked(r.unit, r.team, team);
			} while ((r = check_for_captures(y, x, team)), r.team != empty);

			board[y * board_width + x] = team;
			form_unit_for_team(y, x, team, temporary_data);

			return true;
		}

		return false;
		/*
		if (r.team == empty && spaces_free_around(y,x) == 0) {
			// not a legal move if the space is surrounded
			return; 
		} else {

		}
		*/
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

int main(int argc, char** argv)
{
	uint32_t window_width = 1920;
	uint32_t window_height = 1080;
	glfw3_setup_t r = glfw3_setup(window_width, window_height);

	bool show_demo_window = true;

	go_ctx go_game = {};
	/*
	uint32_t x_tiles = 9;
	uint32_t y_tiles = 9;
	*/
	go_game.initialize_board();

	// wood dbaf67
	ImU32 gray = ImU32{ 0xffc1c0c1 };
	ImU32 dark_gray = ImU32{ 0xff4d4d4d };
	ImU32 red = ImU32{ 0xff2828f2 };

	ImU32 black = ImU32{ 0xff030303 };
	ImU32 wood = ImU32{ 0xff67afdb };

	std::array<ImU32, 9> team_color_palette = {
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

		int width;
		int height;
		glfwGetFramebufferSize(r.window, &width, &height);
		ImGui::SetNextWindowSize(ImVec2(width, height)); // ensures ImGui fits the GLFW window

		int tile_xdim = width / (go_game.board_width + 1);
		int tile_ydim = height / (go_game.board_height + 1);

		int tile_dim = std::min(tile_xdim, tile_ydim);

		ImGui::SetNextWindowPos(ImVec2(0, 0));
		{
			ImGui::Begin("go", nullptr, ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_::ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse
			);

			float line_width = tile_dim / 20.0f;

			ImVec2 mouse = ImGui::GetMousePos();

			bool ok_mouse = ImGui::IsMousePosValid(&mouse);

			//ImVec2 grid_top_left = { (float)0 * tile_dim, (float)0 * tile_dim };
			//ImVec2 grid_btm_right = { (float)(go_game.board_width + 1) * tile_dim, (float)(go_game.board_height + 1) * tile_dim };
			
			//bool mouse_in_grid = ImGui::IsMouseHoveringRect(grid_top_left, grid_btm_right);

			bool left_clicked = ok_mouse && ImGui::IsMouseClicked(ImGuiMouseButton_::ImGuiMouseButton_Left);
			bool right_clicked = ok_mouse && ImGui::IsMouseClicked(ImGuiMouseButton_::ImGuiMouseButton_Right);

			ImDrawList* draw_list = ImGui::GetWindowDrawList();

			// background
			draw_list->AddRectFilled(ImVec2{ (float)tile_dim / 2, (float)tile_dim / 2 },
				ImVec2{ (float)tile_dim * go_game.board_width + (tile_dim / 2), (float)tile_dim * go_game.board_height + (tile_dim / 2)},
				wood);


			// draw lines
			for (size_t i = 1; i < (go_game.board_width + 1); i++) {
				draw_list->AddLine(ImVec2{ (float)i * tile_dim, (float)tile_dim },
					ImVec2{ (float)i * tile_dim, (float)go_game.board_height * tile_dim }, black, line_width);
			}

			for (size_t i = 1; i < (go_game.board_height + 1); i++) {
				draw_list->AddLine(ImVec2{ (float)tile_dim, (float)i * tile_dim },
					ImVec2{ (float)go_game.board_width * tile_dim, (float)i * tile_dim }, black, line_width);
			}

			// fixup grid corners
			draw_list->AddRect(ImVec2{ (float)tile_dim, (float)tile_dim },
				ImVec2{ (float)tile_dim * go_game.board_width, (float)tile_dim * go_game.board_height },
				black, 0.0f, 0, line_width * 1.1f
				);

			//19x19 (19 / 2) -> 9, (9 / 2) -> 4
			//0 1 2 3 ||4|| 5 6 7 8 |9| 10 11 12 13 ||14|| 15 16 17 18
			
			// star points
			uint32_t mid_star_y  = go_game.board_height / 2;
			uint32_t low_star_y  = mid_star_y / 2;
			uint32_t high_star_y = (go_game.board_height -1) - low_star_y;

			uint32_t mid_star_x = go_game.board_width / 2;
			uint32_t low_star_x = mid_star_x / 2;
			uint32_t high_star_x = (go_game.board_width -1) - low_star_x;

			{
				draw_list->AddCircleFilled(
					ImVec2{ (float)tile_dim * (low_star_y + 1), (float)tile_dim * (low_star_x + 1) }, (line_width * 2.0f),
					black);
				/*
				draw_list->AddCircleFilled(
					ImVec2{ (float)tile_dim * (low_star_y + 1), (float)tile_dim * (mid_star_x + 1) }, (line_width * 2.5f),
					black);
					*/
				draw_list->AddCircleFilled(
					ImVec2{ (float)tile_dim * (low_star_y + 1), (float)tile_dim * (high_star_x + 1) }, (line_width * 2.0f),
					black);

				/*
				draw_list->AddCircleFilled(
					ImVec2{ (float)tile_dim * (mid_star_y + 1), (float)tile_dim * (low_star_x + 1) }, (line_width * 2.5f),
					black);
					*/
				draw_list->AddCircleFilled(
					ImVec2{ (float)tile_dim * (mid_star_y + 1), (float)tile_dim * (mid_star_x + 1) }, (line_width * 2.0f),
					black);
				/*
				draw_list->AddCircleFilled(
					ImVec2{ (float)tile_dim * (mid_star_y + 1), (float)tile_dim * (high_star_x + 1) }, (line_width * 2.5f),
					black);
					*/
				draw_list->AddCircleFilled(
					ImVec2{ (float)tile_dim * (high_star_y + 1), (float)tile_dim * (low_star_x + 1) }, (line_width * 2.0f),
					black);
				/*
				draw_list->AddCircleFilled(
					ImVec2{ (float)tile_dim * (high_star_y + 1), (float)tile_dim * (mid_star_x + 1) }, (line_width * 2.5f),
					black);
					*/
				draw_list->AddCircleFilled(
					ImVec2{ (float)tile_dim * (high_star_y + 1), (float)tile_dim * (high_star_x + 1) }, (line_width * 2.0f),
					black);
			}

			for (size_t y = 0; y < go_game.board_height; y++) {
				for (size_t x = 0; x < go_game.board_width; x++) {
					ImVec2 top_left = { (float)tile_dim * (x + 1) - (tile_dim / 4), (float)tile_dim * (y + 1) - (tile_dim/4) };
					ImVec2 bottom_right = { (float)tile_dim * (x + 1) + (tile_dim / 4), (float)tile_dim * (y + 1) + (tile_dim/4) };

					bool is_hovering = ImGui::IsMouseHoveringRect(top_left, bottom_right);

					bool placed_marker = false; //go_game.board[y * go_game.board_width + x] != go_ctx::empty;

					if (!placed_marker && is_hovering && left_clicked) {
						placed_marker = go_game.place_marker(y, x, 1ull);
					}
					else if (!placed_marker && is_hovering && right_clicked) {
						placed_marker = go_game.place_marker(y, x, 2ull);
					}

					if (go_game.board[y * go_game.board_width + x] != go_ctx::empty) {
						draw_list->AddCircleFilled(
							ImVec2{ (float)tile_dim * (x + 1), (float)tile_dim * (y + 1) },
							((tile_dim*3) / 8),
							team_color_palette[go_game.board[y * go_game.board_width + x] % team_color_palette.size()]);
					}
					// if placed marker -> alternate teams
				}
			}



			ImGui::End();
		}

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

// boilerplate ~for windows build~
int WinMain(int argc, char** argv) {
	return main(argc, argv);
}