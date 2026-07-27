-- enable commonlib-shared's REX::INI settings support (pulls simpleini)
set_config("commonlib_ini", true)

-- pull in CommonLibSF (and its commonlib-shared submodule) from the shared
-- SDK clone two levels up (this project lives in M:\Starfield\Mods\<name>)
includes("../../commonlibsf")

-- set minimum xmake version
set_xmakever("3.0.0")

-- set project constants
set_project("ShipNavPanel")
set_version("0.3.8")
set_license("GPL-3.0-or-later")

set_arch("x64")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

target("ShipNavPanel", function()
    add_rules("commonlibsf.plugin", {
        name = "ShipNavPanel",
        author = "frstwlf",
        description = "Points a labelled arrow at any planet in the system while cruising."
    })

    add_files("src/**.cpp")
    add_includedirs("src")

    -- ship the default config next to the DLL
    add_installfiles("ShipNavPanel.ini", { prefixdir = "SFSE/Plugins" })

    -- without XSE_SF_GAME_PATH / XSE_SF_MODS_PATH set, deploy into the build tree
    if not os.getenv("XSE_SF_MODS_PATH") and not os.getenv("XSE_SF_GAME_PATH") then
        set_installdir("$(builddir)/deploy/Data")
    end
end)
