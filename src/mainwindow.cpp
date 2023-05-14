#include "mainwindow.hpp"
#include "gempyre.h"
#include "music_player.hpp"
#include "tagreader.hpp"
#include "ahang_utils.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <gempyre_client.h>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <ios>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <gempyre_utils.h>
#include <thread>
#include <fstream>
#include <tuple>
#include <vector>
#include <mutex>
// #define ahang_debug
constexpr const std::string scale_smaller = "scale(0.88)";

mywindow::mywindow(const Filemap& f,const std::string &index,const std::string &title,const int width, const int height):

#ifndef ahang_debug
Gempyre::Ui(f,index,title,width,height),
 #else
//Gempyre::Ui(f,index,"","debug=True"),
Gempyre::Ui(f,index,"xdg-open ",""),//fake constructor for debug purposes
#endif

//Gempyre::Ui(f,index,"librewolf ",""),//fake constructor for debug purposes
About(f,"about.html","About",500,220,Gempyre::Ui::NoResize),
songlist(*this, "songlist"),
bgblur(*this,"background-blur"),
open_button(*this, "open"),
stop_button(*this,"stopbutton"),
play_button(*this,"playbutton"),
lightdark_button(*this,"lightdark"),
volume_slider(*this,"vol"),
seeker(*this,"seeker"),
overviewcontainer(*this,"songoverview"),
songnameinoverview(*this,"songnameinoverview"),
coverartinoverview(*this,"coverartinoverview"),
debuginfo_button(*this,"button",this->root()),
stock_coverart("song.png"),
playerbar(*this,"playerbar"),
about_button(*this,"aboutbutton")
{
    debuginfo_button.set_style("position", "fixed");

    #ifdef ahang_debug
        debuginfo_button.set_html("D");
        debuginfo_button.subscribe("click", std::bind(&mywindow::on_dbginfoclicked,this));
        debuginfo_button.set_style("right", "10px");
        debuginfo_button.set_style("top", "10px");
        debuginfo_button.set_style("font-size", "1.5rem");
    #else 
        debuginfo_button.set_style("display", "none");
    #endif
    open_button.subscribe("click", std::bind(&mywindow::onopenbuttonclicked,this));
    stop_button.subscribe("click", std::bind(&mywindow::onstopclicked,this));
    play_button.subscribe("click", std::bind(&mywindow::onplaypause_clicked,this));
    volume_slider.subscribe("input", std::bind(&mywindow::onvolumesliderchanged,this,std::placeholders::_1));
    lightdark_button.subscribe("click", std::bind(&mywindow::ondarklightbtn_clicked,this,std::placeholders::_1));
    seeker.subscribe("input", std::bind(&mywindow::onuserchangedseeker,this,std::placeholders::_1));
    this->start_periodic(200ms,std::bind(&mywindow::update_seeker_pos,this,std::placeholders::_1));
    this->set_timer_on_hold(true);
    if(GempyreUtils::current_os()==GempyreUtils::OS::WinOs) about_button.subscribe("click",
    std::bind(&mywindow::alert,this,"Ahang\nSimple music player.\nSource code and donation:\nhttps://github.com/master811129/ahang"));
    else about_button.subscribe("click", std::bind(&Gempyre::Ui::run,&About));
    music_player.set_volume(50);
}


void mywindow::onopenbuttonclicked()
{
    auto dialogpath = Gempyre::Dialog::open_dir_dialog();
    if(dialogpath)
    {
        std::filesystem::path dir = dialogpath.value();
        for(auto &song : songs)std::get<0>(song)->remove();
        songs.clear();
        auto start = std::chrono::high_resolution_clock::now();
        for(const auto &entry : std::filesystem::recursive_directory_iterator(dir))
        {
            if(entry.is_regular_file() && (entry.path().filename().string()[0]!='.') && ahang::is_supported(entry.path())) 
            //if file is not a dir and not hidden and playable
            {
                songs.emplace_back(nullptr,tagreader(entry.path()),entry.path());//element - tag class - path
            }
        }
        std::ranges::sort(songs,[](auto& p,auto& s){
            return std::filesystem::last_write_time(std::get<2>(p))>std::filesystem::last_write_time(std::get<2>(s));
        });
        //TODO sort by other values;
        for(auto &song:songs)
        {
            //auto& below: Passing by reference is very important because we want to make a shared pointer. 
            //if we copy then make_shared, the pointer will be destroyed after we quit the scope.
            auto& [element,tag,filepath] = song;
            element=std::make_shared<Gempyre::Element>(*this,"div",songlist);
            element->set_attribute("class","song");
            const auto title = tag.title();
            const auto artist = tag.artist();
            const std::string picsrc (tag.get_pic()?
                "data:image/png;base64, "+GempyreUtils::base64_encode(reinterpret_cast<const unsigned char*>(tag.get_pic().value()), tag.pic_size()):
                stock_coverart
            );    
            element->set_html(
            "<img src='"+picsrc+"' >"
            "<div class='songdetails'>"
                "<h3>" + title + "</h3>"
                "<p>" + artist + "</p>"
            "</div>"
            );
            element->subscribe("click", std::bind(&mywindow::ononesongentryclicked,this,song));
        }
        
        auto stop =  std::chrono::high_resolution_clock::now();
        std::cout << "indexed in: " << std::chrono::duration_cast<std::chrono::milliseconds>(stop-start).count() << "ms" << std::endl;
        songs.emplace_back(std::make_shared<Gempyre::Element>(*this,"div",songlist),"","");//space at the end of the list
        std::get<0>(songs[songs.size()-1])->set_attribute("class","lastspace");
    }
}


void mywindow::ononesongentryclicked(std::tuple<std::shared_ptr<Gempyre::Element>,tagreader,std::filesystem::path> &song)
{
    auto&[element,tag,filepath] = song;
    const std::filesystem::path filepath_cpy = filepath;//for using in thread
    std::thread play ([this,filepath_cpy]
    {
        std::lock_guard<std::mutex> a(this->mutex) ;
        music_player.play(filepath_cpy);//it cancels the previous song automatically if playing 
        this->set_timer_on_hold(false);
        std::thread opusworkaround([this,filepath_cpy]{
            std::this_thread::sleep_for(100ms);
            if(!music_player.is_active())
            {
                if (filepath_cpy.extension()==".opus")
                {
                    songnameinoverview.set_attribute("class","loading-animation");
                    std::stringstream t ;
                    t << std::quoted(filepath_cpy.string());
                    const auto tmp = std::filesystem::temp_directory_path();
                    if(std::filesystem::exists(tmp/"ahang.mp3"))std::filesystem::remove(tmp/"ahang.mp3");
                    auto convert_to_opus_cmd = "ffmpeg -i "+ t.str() +" -acodec mp3 " + (tmp/"ahang.mp3").string();
                    ahang::system(convert_to_opus_cmd);
                    music_player.play(tmp/"ahang.mp3");
                    this->set_timer_on_hold(false);
                    coverartinoverview.set_style("transform", "scale(1)");
                }
            }
            else 
            {
                coverartinoverview.set_style("transform", "scale(1)");// I dont think this is safe
            }
            songnameinoverview.set_attribute("class","");
        });
        opusworkaround.detach();
    });

    play.detach();
    const auto title = tag.title();
    songnameinoverview.set_html(title);
    if(tag.get_pic())
    {
        auto picsrc = "data:image/png;base64, "+GempyreUtils::base64_encode(reinterpret_cast<const unsigned char*>(tag.get_pic().value()), tag.pic_size());
        coverartinoverview.set_attribute("src",picsrc);
        bgblur.set_style("background-image", "url('"+picsrc+"')");
    }
    else 
    {
        coverartinoverview.set_attribute("src",stock_coverart);
        bgblur.set_style("background-image","url('"+stock_coverart+"')");
    }
}

void mywindow::onstopclicked(void)
{
    music_player.stop();
    seeker.set_attribute("value","0");
    coverartinoverview.set_style("transform", scale_smaller);
}

void mywindow::onplaypause_clicked()
{
    //std::cout << "Play/Pause: Will change MusicPlayer class instance to state: " << (!music_player.is_paused()?"PAUSE":"PLAY") << std::endl;
    music_player.toggle_pause();
    if(music_player.is_paused())coverartinoverview.set_style("transform", scale_smaller);
    else coverartinoverview.set_style("transform", "scale(1)");
    if(!music_player.is_active())
    {//that means we have a song that already has been played and we want to replay it. 
    ///If it is not the case MusicPlayer class will handle it automatically.(does nothing)
        music_player.play();
        this->set_timer_on_hold(false);
    }
}

void mywindow::update_seeker_pos(Gempyre::Ui::TimerId id)
{
    seeker.set_attribute("value",std::to_string(music_player.get_position()));
    if(!music_player.is_active())
    {
        this->set_timer_on_hold(true);
        coverartinoverview.set_style("transform", scale_smaller);
    }
}

void mywindow::onuserchangedseeker(const Gempyre::Event & s)
{
    music_player.seek( std::stoi(s.element.values()->at("value")));
}


void mywindow::onvolumesliderchanged(const Gempyre::Event& slider_ref)
{
    auto vol = std::stof( slider_ref.element.values()->at("value"));
    music_player.set_volume(vol);
}

void mywindow::ondarklightbtn_clicked(const Gempyre::Event& e)
{
    this->toggledark(e.element.values()->at("checked")=="true");
}

void mywindow::toggledark(bool is_dark)
{
    auto body = this->root();
    //Deceleraing Gempyre Element and then distroying it makes the UI library crazy.
    if(is_dark)stock_coverart="song.png";
    else stock_coverart="song-light.png";
    //element attr light dark
    std::array<std::tuple<std::reference_wrapper<Gempyre::Element>,const std::string,const std::string,const std::string> ,12> lightcolorscheme{{
        {body,"color-scheme","light","dark"}, //Chromium does not respect user prefrence so I do.
        {body,"background","#d5d5d5",""},
        {body,"color","hsl(0deg, 0%, 21%)",""},
        {songlist,"background", "#eaeaeaa3",""},
        {bgblur,"filter","blur(40px) opacity(0.8)",""},
        {play_button,"filter","invert(1)",""},
        {stop_button,"filter","invert(1)",""},
        {open_button,"filter","invert(1)",""},
        {lightdark_button, "filter","invert(1)",""},
        {about_button, "filter","invert(1)",""},
        {overviewcontainer,"background", "#eaeaeaa3",""},
        {coverartinoverview, "border", "1px solid #999999",""}
    }};
    for(const auto& scheme:lightcolorscheme)
    {
        auto& [e,k,lightval,darkval] = scheme;
        std::array<std::reference_wrapper<const std::string>,2> lightdark{{lightval,darkval}};
        e.get().set_style(k, lightdark[is_dark]);
    }
    for(auto &song:songs)
    {
        auto &[element,tag,path]=song;
        if(!tag.get_pic() && !path.empty())
        {
            element->set_html(
            "<img src='"+stock_coverart+"' >"
            "<div class='songdetails'>"
                "<h3>" + ((!tag.title().empty())? tag.title(): std::string("Not Kown")) + "</h3>"
                "<p>" + ((!tag.artist().empty())? tag.artist(): std::string("Not Kown")) + "</p>"
            "</div>"
            );
        }
    }
    const auto src = coverartinoverview.attributes().value()["src"];
    if(src == "song.png" || src == "song-light.png")
    {
        coverartinoverview.set_attribute("src",stock_coverart);
        bgblur.set_style("background-image","url('"+stock_coverart+"')");
    }
    
    this->eval(is_dark?"document.documentElement.style.setProperty('--song-hvr-bg-color', '');"  :
                       "document.documentElement.style.setProperty('--song-hvr-bg-color', '#d4d4d4');");
    
}


void mywindow::on_dbginfoclicked()//hidden button
{
    const auto [first_gem_ver,second_gem_ver,third_gem_ver] = Gempyre::version();
    std::cout << std::boolalpha << "-------------🚧DEBUG INFO🚧-------------" << 
    "\nGempyre version: " << first_gem_ver << '.' << second_gem_ver << '.' <<third_gem_ver <<
    "\nSoloud Version: " << music_player.pass_engine()->getVersion() <<
    "\nNumber of indexed files: " << (songs.size()==0?songs.size(): songs.size()-1)  <<
    "\nVolume from MusicPlayer class: " << music_player.pass_engine()->getGlobalVolume() << 
    "\nstream position: " << music_player.get_position() << '%' <<
    "\nis there any active stream? " << music_player.is_active() <<
    "\nis stream paused? " << music_player.is_paused() <<
    "\nis stream playing? " << music_player.is_playing() <<
    "\n--------------------------------------\n" << std::endl;
}