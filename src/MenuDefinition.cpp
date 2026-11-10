#include "ModMenu/App.h"
#include "ModMenu/BridgeContracts.h"
#include <set>
namespace be::native {
bool InsertModsDefinition(std::string const& input, std::string& output, std::string_view expected) {
    try {
        if (input.size()>1024*1024 || (expected!="start" && expected!="pause")) return false;
        std::vector<std::set<std::string>> seen;
        auto parser=[&](int depth, Json::parse_event_t event, Json& node) {
            if(depth>64) throw std::runtime_error("nested UI");
            if(event==Json::parse_event_t::object_start) seen.emplace_back();
            if(event==Json::parse_event_t::key && !seen.back().insert(node.get<std::string>()).second) throw std::runtime_error("duplicate key");
            if(event==Json::parse_event_t::object_end) seen.pop_back();
            return true;
        };
        auto page=Json::parse(input,parser,true,true);
        if(!page.is_object() || page.value("namespace",std::string{})!=expected) return false;
        if(page.dump().find(ModsEventName)!=std::string::npos) { output=input; return true; }
        Json button={{"$button_text","Mods"},{"$pressed_button_name",ModsEventName},{"$focus_id","be_mods"}};
        button["button_mappings"]=Json::array();
        for(auto from : {"button.menu_select","button.menu_ok"})
            button["button_mappings"].push_back({{"from_button_id",from},{"to_button_id","$pressed_button_name"},{"mapping_type","pressed"}});
        Json* rows=nullptr;
        std::string marker, definition;
        Json row;
        if(expected=="start") {
            auto& controls=page.at("main_buttons_panel").at("controls");
            for(auto& item:controls) if(item.contains("stacked_rows")) {
                if(rows) return false;
                auto& stack=item.at("stacked_rows");
                if(stack.value("type",std::string{})!="stack_panel") return false;
                rows=&stack.at("controls");
            }
            marker="play@start.play_button_stack";
            definition="be_mods_button@start.main_button";
            row={{"be_mods_row@start.stacked_row",{{"controls",Json::array({{{"be_mods@start.be_mods_button",Json::object()}}})}}}};
        } else {
            auto& controls=page.at("menu_background@pause.transparent_background").at("controls");
            for(auto& item:controls) if(item.contains("button_panel")) {
                if(rows) return false;
                auto& stack=item.at("button_panel");
                if(stack.value("type",std::string{})!="stack_panel") return false;
                rows=&stack.at("controls");
            }
            marker="settings_button@pause.settings_button";
            // Reuse the same sizing, anchors and focus navigation as Settings.
            // Keep the relative width from the game's template, not a pixel width.
            definition="be_mods_button@pause.settings_button";
            button["size"]=page.at("pause_button_template@common_buttons.light_text_button").at("size");
            button["focus_identifier"]="be_pause_mods";
            button["anchor_from"]="top_middle"; button["anchor_to"]="top_middle";
            row={{"be_mods@pause.be_mods_button",Json::object()}};
        }
        if(!rows || !rows->is_array() || rows->size()>256 || page.contains(definition)) return false;
        std::size_t found=rows->size();
        for(std::size_t i=0;i<rows->size();++i) if((*rows)[i].contains(marker)) {
            if(found!=rows->size()) return false;
            found=i;
        }
        if(found==rows->size()) return false;
        auto insert=found+1;
        if(expected=="pause" && insert<rows->size() && (*rows)[insert].contains("settings@pause.vertical_padding")) ++insert;
        rows->insert(rows->begin()+Json::difference_type(insert),row);
        if(expected=="pause") {
            // A button's border extends beyond its content. Without the normal
            // spacer the following button overlaps that border and lower lip.
            Json padding={{"be_mods_padding@pause.vertical_padding",{{"size",Json::array({0,3})}}}};
            rows->insert(rows->begin()+Json::difference_type(insert+1),padding);
        }
        page[definition]=std::move(button);
        output=page.dump();
        return output.size()<=1024*1024;
    } catch(...) { return false; }
}
}
