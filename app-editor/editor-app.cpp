#include "index.hpp"
#include "editor-app.hpp"
#include "editor-ui.hpp"
#include "serialization.inl"
#include "logging.hpp"
#include "win32.hpp"

using namespace polymer;

// The Polymer editor has a number of "intrinsic" mesh assets that are loaded from disk at runtime. These primarily
// add to the number of objects that can be quickly prototyped with, along with the usual set of procedural mesh functions
// included with Polymer.
void load_editor_intrinsic_assets(path root)
{
    scoped_timer t("load_editor_intrinsic_assets");
    for (auto & entry : recursive_directory_iterator(root))
    {
        const size_t root_len = root.string().length(), ext_len = entry.path().extension().string().length();
        auto path = entry.path().string(), name = path.substr(root_len + 1, path.size() - root_len - ext_len - 1);
        for (auto & chr : path) if (chr == '\\') chr = '/';

        if (entry.path().extension().string() == ".mesh")
        {
            auto geo_import = import_mesh_binary(path);
            create_handle_for_asset(std::string("poly-" + get_filename_without_extension(path)).c_str(), make_mesh_from_geometry(geo_import));
            create_handle_for_asset(std::string("poly-" + get_filename_without_extension(path)).c_str(), std::move(geo_import));
        }
    }
};

scene_editor_app::scene_editor_app() : polymer_app(1920, 1080, "Polymer Editor")
{
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    int width, height;
    glfwGetWindowSize(window, &width, &height);
    glViewport(0, 0, width, height);

    auto droidSansTTFBytes = read_file_binary("../assets/fonts/droid_sans.ttf");

    igm.reset(new gui::imgui_instance(window));
    gui::make_light_theme();
    igm->add_font(droidSansTTFBytes);

    gizmo_selector.reset(new selection_controller<GameObject>());

    cam.look_at({ 0, 9.5f, -6.0f }, { 0, 0.1f, 0 });
    flycam.set_camera(&cam);

    log::get()->add_sink(std::make_shared<ImGui::spdlog_editor_sink>(log));

    load_editor_intrinsic_assets("../assets/models/runtime/");

    shaderMonitor.watch("wireframe",
        "../assets/shaders/wireframe_vert.glsl",
        "../assets/shaders/wireframe_frag.glsl",
        "../assets/shaders/wireframe_geom.glsl",
        "../assets/shaders/renderer");

    shaderMonitor.watch("ibl", 
        "../assets/shaders/ibl_vert.glsl",
        "../assets/shaders/ibl_frag.glsl",
        "../assets/shaders/renderer");

    shaderMonitor.watch("depth-prepass",
        "../assets/shaders/renderer/depth_prepass_vert.glsl",
        "../assets/shaders/renderer/depth_prepass_frag.glsl",
        "../assets/shaders/renderer");

    shaderMonitor.watch("default-shader",
        "../assets/shaders/renderer/forward_lighting_vert.glsl",
        "../assets/shaders/renderer/default_material_frag.glsl",
        "../assets/shaders/renderer");

    shaderMonitor.watch("post-tonemap",
        "../assets/shaders/renderer/post_tonemap_vert.glsl",
        "../assets/shaders/renderer/post_tonemap_frag.glsl");

    shaderMonitor.watch("cascaded-shadows",
        "../assets/shaders/renderer/shadowcascade_vert.glsl",
        "../assets/shaders/renderer/shadowcascade_frag.glsl",
        "../assets/shaders/renderer/shadowcascade_geom.glsl",
        "../assets/shaders/renderer");

    shaderMonitor.watch("pbr-forward-lighting",
        "../assets/shaders/renderer/forward_lighting_vert.glsl",
        "../assets/shaders/renderer/forward_lighting_frag.glsl",
        "../assets/shaders/renderer");

    shaderMonitor.watch("blinn-phong",
        "../assets/shaders/renderer/forward_lighting_vert.glsl",
        "../assets/shaders/renderer/forward_lighting_blinn_phong_frag.glsl",
        "../assets/shaders/renderer");

    fullscreen_surface.reset(new fullscreen_texture());

    renderer_settings settings;
    settings.renderSize = int2(width, height);
    renderer.reset(new forward_renderer(settings));

    sceneData.ibl_irradianceCubemap = "wells-irradiance-cubemap";
    sceneData.ibl_radianceCubemap = "wells-radiance-cubemap";

    scene.skybox.reset(new HosekProceduralSky());
    sceneData.skybox = scene.skybox.get();
    scene.skybox->onParametersChanged = [&]
    {
        uniforms::directional_light updatedSun;
        updatedSun.direction = scene.skybox->get_sun_direction();
        updatedSun.color = float3(1.f, 1.0f, 1.0f);
        updatedSun.amount = 1.0f;
        sceneData.sunlight = updatedSun;
    };
    scene.skybox->onParametersChanged(); // call for initial set

    // fixme to be resolved
    auto radianceBinary = read_file_binary("../assets/textures/envmaps/wells_radiance.dds");
    auto irradianceBinary = read_file_binary("../assets/textures/envmaps/wells_irradiance.dds");
    gli::texture_cube radianceHandle(gli::load_dds((char *)radianceBinary.data(), radianceBinary.size()));
    gli::texture_cube irradianceHandle(gli::load_dds((char *)irradianceBinary.data(), irradianceBinary.size()));
    create_handle_for_asset("wells-radiance-cubemap", load_cubemap(radianceHandle));
    create_handle_for_asset("wells-irradiance-cubemap", load_cubemap(irradianceHandle));

    scene.objects.clear();
    cereal::deserialize_from_json("../assets/scene.json", scene.objects);

    scene.materialLib.reset(new polymer::material_library("../assets/materials.json"));

    // Resolve asset_handles to resources on disk
    resolver.reset(new asset_resolver());
    resolver->resolve("../assets/", &scene, scene.materialLib.get());

    // Setup Debug visualizations
    uiSurface.bounds = { 0, 0, (float)width, (float)height };
    uiSurface.add_child({ { 0.0000f, +20 },{ 0, +20 },{ 0.1667f, -10 },{ 0.133f, +10 } });
    uiSurface.add_child({ { 0.1667f, +20 },{ 0, +20 },{ 0.3334f, -10 },{ 0.133f, +10 } });
    uiSurface.add_child({ { 0.3334f, +20 },{ 0, +20 },{ 0.5009f, -10 },{ 0.133f, +10 } });
    uiSurface.add_child({ { 0.5000f, +20 },{ 0, +20 },{ 0.6668f, -10 },{ 0.133f, +10 } });
    uiSurface.layout();

    debugViews.push_back(std::make_shared<GLTextureView>(true));
    debugViews.push_back(std::make_shared<GLTextureView>(true, float2(cam.nearclip, cam.farclip)));
}

scene_editor_app::~scene_editor_app()
{ 

}

void scene_editor_app::on_drop(std::vector<std::string> filepaths)
{
    for (auto & path : filepaths)
    {
        std::transform(path.begin(), path.end(), path.begin(), ::tolower);
        const std::string fileExtension = get_extension(path);

        if (fileExtension == "png" || fileExtension == "tga" || fileExtension == "jpg")
        {
            create_handle_for_asset(get_filename_without_extension(path).c_str(), load_image(path, false));
            return;
        }

        auto importedModel = import_model(path);

        for (auto & m : importedModel)
        {
            auto & mesh = m.second;
            rescale_geometry(mesh, 1.f);

            if (mesh.normals.size() == 0) compute_normals(mesh);
            if (mesh.tangents.size() == 0) compute_tangents(mesh);
                
            const std::string filename = get_filename_without_extension(path);
            const std::string outputBasePath = "../assets/models/runtime/";
            const std::string outputFile = outputBasePath + filename + "-" + m.first + "-" + ".mesh";

            export_mesh_binary(outputFile, mesh, false);

            auto importedMesh = import_mesh_binary(outputFile);

            create_handle_for_asset(std::string(get_filename_without_extension(path) + "-" + m.first).c_str(), make_mesh_from_geometry(importedMesh));
            create_handle_for_asset(std::string(get_filename_without_extension(path) + "-" + m.first).c_str(), std::move(importedMesh));
        }
    }
}

void scene_editor_app::on_window_resize(int2 size) 
{ 
    uiSurface.bounds = { 0, 0, (float)size.x, (float)size.y };
    uiSurface.layout();

    // Iconification/minimization triggers an on_window_resize event with a zero size
    if (size.x > 0 && size.y > 0)
    {
        renderer_settings settings;
        settings.renderSize = size;
        reset_renderer(size, settings);
    }
}

void scene_editor_app::on_input(const app_input_event & event)
{
    igm->update_input(event);
    gizmo_selector->on_input(event);

    if (ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard)
    {
        flycam.reset();
        gizmo_selector->reset_input();
        return;
    }

    // flycam only works when a mod key isn't held down
    if (event.mods == 0) flycam.handle_input(event);

    {
        if (event.type == app_input_event::KEY)
        {
            // De-select all objects
            if (event.value[0] == GLFW_KEY_ESCAPE && event.action == GLFW_RELEASE)
            {
                gizmo_selector->clear();
            }

            // Focus on currently selected object
            if (event.value[0] == GLFW_KEY_F && event.action == GLFW_RELEASE)
            {
                if (gizmo_selector->get_selection().size() == 0) return;
                if (auto * sel = gizmo_selector->get_selection()[0])
                {
                    auto selectedObjectPose = sel->get_pose();
                    auto focusOffset = selectedObjectPose.position + float3(0.f, 0.5f, 4.f);
                    cam.look_at(focusOffset, selectedObjectPose.position);
                    flycam.update_yaw_pitch();
                }
            }

            // Togle drawing ImGUI
            if (event.value[0] == GLFW_KEY_TAB && event.action == GLFW_RELEASE)
            {
                showUI = !showUI;
            }

            if (event.value[0] == GLFW_KEY_SPACE && event.action == GLFW_RELEASE)
            {
                if (!material_editor)
                {
                    material_editor.reset(new material_editor_window(get_shared_gl_context(), 500, 1200, "", 1, *scene.materialLib.get(), *gizmo_selector.get()));
                }
                else if (!material_editor->get_window())
                {
                    // Workaround since there's no convenient way to reset the material_editor when it's been closed
                    material_editor.reset(new material_editor_window(get_shared_gl_context(), 500, 1200, "", 1, *scene.materialLib.get(), *gizmo_selector.get()));
                }
            }
        }

        // Raycast for editor/gizmo selection on mouse up
        if (event.type == app_input_event::MOUSE && event.action == GLFW_RELEASE && event.value[0] == GLFW_MOUSE_BUTTON_LEFT)
        {
            int width, height;
            glfwGetWindowSize(window, &width, &height);

            const Ray r = cam.get_world_ray(event.cursor, float2(width, height));

            if (length(r.direction) > 0 && !gizmo_selector->active())
            {
                std::vector<GameObject *> selectedObjects;
                float best_t = std::numeric_limits<float>::max();
                GameObject * hitObject = nullptr;

                for (auto & obj : scene.objects)
                {
                    RaycastResult result = obj->raycast(r);
                    if (result.hit)
                    {
                        if (result.distance < best_t)
                        {
                            best_t = result.distance;
                            hitObject = obj.get();
                        }
                    }
                }

                if (hitObject) selectedObjects.push_back(hitObject);

                // New object was selected
                if (selectedObjects.size() > 0)
                {
                    // Multi-selection
                    if (event.mods & GLFW_MOD_CONTROL)
                    {
                        auto existingSelection = gizmo_selector->get_selection();
                        for (auto s : selectedObjects)
                        {
                            if (!gizmo_selector->selected(s)) existingSelection.push_back(s);
                        }
                        gizmo_selector->set_selection(existingSelection);
                    }
                    // Single Selection
                    else
                    {
                        gizmo_selector->set_selection(selectedObjects);
                    }
                }
            }

        }
    }
}

void scene_editor_app::reset_renderer(int2 size, const renderer_settings & settings)
{
    renderer.reset(new forward_renderer(settings));
}

void scene_editor_app::on_update(const app_update_event & e)
{
    int width, height;
    glfwGetWindowSize(window, &width, &height);

    editorProfiler.begin("on_update");
    flycam.update(e.timestep_ms);
    shaderMonitor.handle_recompile();
    gizmo_selector->on_update(cam, float2(width, height));
    editorProfiler.end("on_update");
}

void scene_editor_app::on_draw()
{
    glfwMakeContextCurrent(window);

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);

    int width, height;
    glfwGetWindowSize(window, &width, &height);

    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const Pose cameraPose = cam.pose;
    const float4x4 projectionMatrix = cam.get_projection_matrix(float(width) / float(height));
    const float4x4 viewMatrix = cam.get_view_matrix();
    const float4x4 viewProjectionMatrix = mul(projectionMatrix, viewMatrix);

    {
        editorProfiler.begin("gather-scene");

        // Remember to clear any transient per-frame data
        sceneData.pointLights.clear();
        sceneData.renderSet.clear();
        sceneData.views.clear();

        // Gather Lighting
        for (auto & obj : scene.objects)
        {
            if (auto * r = dynamic_cast<PointLight*>(obj.get()))
            {
                sceneData.pointLights.push_back(r->data);
            }
        }

        // Gather Objects
        std::vector<Renderable *> sceneObjects;
        for (auto & obj : scene.objects)
        {
            if (auto * r = dynamic_cast<Renderable*>(obj.get()))
            {
                sceneData.renderSet.push_back(r);
            }
        }

        // Single-viewport camera
        sceneData.views.push_back(view_data(0, cameraPose, projectionMatrix));

        editorProfiler.end("gather-scene");

        // Submit scene to the renderer
        editorProfiler.begin("submit-scene");
        renderer->render_frame(sceneData);
        editorProfiler.end("submit-scene");

        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);

        fullscreen_surface->draw(renderer->get_color_texture(0));

        gl_check_error(__FILE__, __LINE__);
    }

    editorProfiler.begin("wireframe-rendering");
    // Selected objects as wireframe
    {
        glDisable(GL_DEPTH_TEST);

        auto & program = wireframeHandle.get()->get_variant()->shader;

        program.bind();

        program.uniform("u_eyePos", cam.get_eye_point());
        program.uniform("u_viewProjMatrix", viewProjectionMatrix);

        for (auto obj : gizmo_selector->get_selection())
        {
            if (auto * r = dynamic_cast<Renderable*>(obj))
            {
                auto modelMatrix = mul(obj->get_pose().matrix(), make_scaling_matrix(obj->get_scale()));
                program.uniform("u_modelMatrix", modelMatrix);
                r->draw();
            }
        }

        program.unbind();

        glEnable(GL_DEPTH_TEST);
    }
    editorProfiler.end("wireframe-rendering");

    editorProfiler.begin("imgui-menu");
    igm->begin_frame();

    gui::imgui_menu_stack menu(*this, ImGui::GetIO().KeysDown);
    menu.app_menu_begin();
    {
        menu.begin("File");
        bool mod_enabled = !gizmo_selector->active();
        if (menu.item("Open Scene", GLFW_MOD_CONTROL, GLFW_KEY_O, mod_enabled))
        {
            const auto selected_open_path = windows_file_dialog("anvil scene", "json", true);
            if (!selected_open_path.empty())
            {
                gizmo_selector->clear();
                scene.objects.clear();
                cereal::deserialize_from_json(selected_open_path, scene.objects);
                glfwSetWindowTitle(window, selected_open_path.c_str());
            }

        }
        if (menu.item("Save Scene", GLFW_MOD_CONTROL, GLFW_KEY_S, mod_enabled))
        {
            const auto save_path = windows_file_dialog("anvil scene", "json", false);
            if (!save_path.empty())
            {
                gizmo_selector->clear();
                write_file_text(save_path, cereal::serialize_to_json(scene.objects));
                glfwSetWindowTitle(window, save_path.c_str());
            }
        }
        if (menu.item("New Scene", GLFW_MOD_CONTROL, GLFW_KEY_N, mod_enabled))
        {
            gizmo_selector->clear();
            scene.objects.clear();
        }
        if (menu.item("Take Screenshot", GLFW_MOD_CONTROL, GLFW_KEY_EQUAL, mod_enabled))
        {
            request_screenshot("scene-editor");
        }
        if (menu.item("Exit", GLFW_MOD_ALT, GLFW_KEY_F4)) exit();
        menu.end();

        menu.begin("Edit");
        if (menu.item("Clone", GLFW_MOD_CONTROL, GLFW_KEY_D)) {}
        if (menu.item("Delete", 0, GLFW_KEY_DELETE)) 
        {
            auto it = std::remove_if(std::begin(scene.objects), std::end(scene.objects), [this](std::shared_ptr<GameObject> obj) 
            { 
                return gizmo_selector->selected(obj.get());
            });
            scene.objects.erase(it, std::end(scene.objects));

            gizmo_selector->clear();
        }
        if (menu.item("Select All", GLFW_MOD_CONTROL, GLFW_KEY_A)) 
        {
            std::vector<GameObject *> selectedObjects;
            for (auto & obj : scene.objects)
            {
                selectedObjects.push_back(obj.get());
            }
            gizmo_selector->set_selection(selectedObjects);
        }
        menu.end();

        menu.begin("Spawn");
        visit_subclasses((GameObject*)0, [&](const char * name, auto * p)
        {
            if (menu.item(name))
            {
                auto obj = std::make_shared<std::remove_reference_t<decltype(*p)>>();
                obj->set_material(material_library::kDefaultMaterialId);
                scene.objects.push_back(obj);

                // Newly spawned objects are selected by default
                std::vector<GameObject *> selectedObjects;
                selectedObjects.push_back(obj.get());
                gizmo_selector->set_selection(selectedObjects);
            }
        });
        menu.end();

    }
    menu.app_menu_end();
    editorProfiler.end("imgui-menu");

    editorProfiler.begin("imgui-editor");
    if (showUI)
    {
        static int horizSplit = 380;
        static int rightSplit1 = (height / 2) - 17;

        // Define a split region between the whole window and the right panel
        auto rightRegion = ImGui::Split({ { 0.f ,17.f },{ (float)width, (float)height } }, &horizSplit, ImGui::SplitType::Right);
        auto split2 = ImGui::Split(rightRegion.second, &rightSplit1, ImGui::SplitType::Top);

        ui_rect topRightPane = { { int2(split2.second.min()) }, { int2(split2.second.max()) } };    // top half
        ui_rect bottomRightPane = { { int2(split2.first.min()) },{ int2(split2.first.max()) } };  // bottom half

        gui::imgui_fixed_window_begin("Entity Inspector", topRightPane);
        if (gizmo_selector->get_selection().size() >= 1)
        {
            inspect_object(nullptr, gizmo_selector->get_selection()[0]);
        }
        gui::imgui_fixed_window_end();

        // Scene Object List
        gui::imgui_fixed_window_begin("Scene Entity List", bottomRightPane);

        for (size_t i = 0; i < scene.objects.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            bool selected = gizmo_selector->selected(scene.objects[i].get());

            // For polymorphic typeids, the trick is to dereference it first
            std::string name = scene.objects[i]->id.size() > 0 ? scene.objects[i]->id : std::string(typeid(*scene.objects[i]).name()); 

            if (ImGui::Selectable(name.c_str(), &selected))
            {
                if (!ImGui::GetIO().KeyCtrl) gizmo_selector->clear();
                gizmo_selector->update_selection(scene.objects[i].get());
            }
            ImGui::PopID();
        }
        gui::imgui_fixed_window_end();

        // Define a split region between the whole window and the left panel
        static int leftSplit = 380;
        static int leftSplit1 = (height / 2);
        auto leftRegionSplit = ImGui::Split({ { 0.f,17.f },{ (float)width, (float)height } }, &leftSplit, ImGui::SplitType::Left);
        auto lsplit2 = ImGui::Split(leftRegionSplit.second, &leftSplit1, ImGui::SplitType::Top);
        ui_rect topLeftPane = { { int2(lsplit2.second.min()) },{ int2(lsplit2.second.max()) } };
        ui_rect bottomLeftPane = { { int2(lsplit2.first.min()) },{ int2(lsplit2.first.max()) } };

        gui::imgui_fixed_window_begin("Renderer", topLeftPane);
        {

            ImGui::Dummy({ 0, 10 });

            if (ImGui::TreeNode("Core"))
            {
                renderer_settings lastSettings = renderer->settings;

                if (build_imgui("renderer", *renderer))
                {
                    renderer->gpuProfiler.set_enabled(renderer->settings.performanceProfiling);
                    renderer->cpuProfiler.set_enabled(renderer->settings.performanceProfiling);

                    /*
                    // fixme
                    if (renderer->settings.shadowsEnabled != lastSettings.shadowsEnabled)
                    {
                        auto & shaderAsset = shaderMonitor.get_asset(pbrProgramAsset);
                        auto & defines = shaderAsset.defines;

                        if (renderer->settings.shadowsEnabled)
                        {
                            // Enable, but check if it's already in there first
                            auto itr = std::find(defines.begin(), defines.end(), "ENABLE_SHADOWS");
                            if (itr == defines.end()) defines.push_back("ENABLE_SHADOWS");

                            auto render_settings_copy = renderer->settings;
                            reset_renderer(renderer->settings.renderSize, render_settings_copy);
                        }
                        else
                        {
                            // Disable
                            auto & defines = shaderMonitor.get_asset(pbrProgramAsset).defines;
                            auto itr = std::find(defines.begin(), defines.end(), "ENABLE_SHADOWS");
                            if (itr != defines.end()) defines.erase(itr);

                            auto render_settings_copy = renderer->settings;
                            reset_renderer(renderer->settings.renderSize, render_settings_copy);
                        }
                        shaderAsset.shouldRecompile = true;
                    }
                    */
                }

                ImGui::TreePop();
            }

            ImGui::Dummy({ 0, 10 });

            if (ImGui::TreeNode("Procedural Sky"))
            {
                inspect_object(nullptr, sceneData.skybox);
                ImGui::TreePop();
            }

            ImGui::Dummy({ 0, 10 });

            if (auto * shadows = renderer->get_shadow_pass())
            {
                if (ImGui::TreeNode("Cascaded Shadow Mapping"))
                {
                    build_imgui("shadows", *shadows);
                    ImGui::TreePop();
                }
            }

            ImGui::Dummy({ 0, 10 });

            if (renderer->settings.performanceProfiling)
            {
                for (auto & t : renderer->gpuProfiler.get_data()) ImGui::Text("[Renderer GPU] %s %f ms", t.first.c_str(), t.second);
                for (auto & t : renderer->cpuProfiler.get_data()) ImGui::Text("[Renderer CPU] %s %f ms", t.first.c_str(), t.second);
            }

            ImGui::Dummy({ 0, 10 });

            for (auto & t : editorProfiler.get_data()) ImGui::Text("[Editor] %s %f ms", t.first.c_str(), t.second);
        }
        gui::imgui_fixed_window_end();

        gui::imgui_fixed_window_begin("Application Log", bottomLeftPane);
        {
            log.Draw("-");
        }
        gui::imgui_fixed_window_end();

    }

    igm->end_frame();
    editorProfiler.end("imgui-editor");

    // Debug Views
    /*
    {
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        debugViews[0]->draw(uiSurface.children[0]->bounds, float2(width, height), renderer->get_output_texture(0));
        debugViews[1]->draw(uiSurface.children[1]->bounds, float2(width, height), renderer->get_output_texture(TextureType::DEPTH, 0));
        glEnable(GL_DEPTH_TEST);
    }
    */

    {
        editorProfiler.begin("gizmo_on_draw");
        glClear(GL_DEPTH_BUFFER_BIT);
        gizmo_selector->on_draw();
        editorProfiler.end("gizmo_on_draw");
    }

    gl_check_error(__FILE__, __LINE__);

    glFlush();
    if (material_editor && material_editor->get_window()) material_editor->run();
    glfwSwapBuffers(window);
}

IMPLEMENT_MAIN(int argc, char * argv[])
{
    try
    {
        scene_editor_app app;
        app.main_loop();
    }
    catch (const std::exception & e)
    {
        std::cerr << "Application Fatal: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}