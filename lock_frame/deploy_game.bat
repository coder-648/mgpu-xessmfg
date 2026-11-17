@echo off
rem ===========================================================================
rem  把 fakerFG 装进一个游戏目录（游戏侧文件）。
rem
rem    deploy_game.bat "F:\steam\steamapps\common\PBZ\platform\steam"
rem
rem  只动游戏侧：fakerFG.dll（两个别名）、xess_mfg_producer.dll、fakerFG.ini。
rem  lock_frame.exe / lock_frame_ui.exe / libxess_fg.dll / libxell.dll 留在
rem  发布包目录里运行，不要复制进游戏目录。
rem ===========================================================================
setlocal
set "GAME=%~1"
if "%GAME%"=="" ( echo usage: deploy_game.bat ^<game exe directory^> & exit /b 1 )
if not exist "%GAME%\" ( echo [deploy] not a directory: %GAME% & exit /b 1 )
set "SRC=%~dp0"

echo [deploy] target: %GAME%

rem --- 1. 把游戏自己的 DLSS-G 运行时改名一次，之后再部署不再改 -------------
if not exist "%GAME%\fakerFG.dll" (
  if exist "%GAME%\nvngx_dlssg.dll" if not exist "%GAME%\nvngx_dlssg_real.dll" (
    echo [deploy] keep the title's DLSS-G runtime as nvngx_dlssg_real.dll
    move /y "%GAME%\nvngx_dlssg.dll" "%GAME%\nvngx_dlssg_real.dll" >nul
  )
)

rem --- 2. 备份游戏原来的 dxgi.dll ------------------------------------------
if exist "%GAME%\dxgi.dll" if not exist "%GAME%\dxgi.dll.bak" (
  echo [deploy] backup dxgi.dll -> dxgi.dll.bak
  move /y "%GAME%\dxgi.dll" "%GAME%\dxgi.dll.bak" >nul
)

rem --- 3. 放我们的文件 ------------------------------------------------------
echo [deploy] fakerFG.dll -> dxgi.dll
copy /y "%SRC%fakerFG.dll" "%GAME%\dxgi.dll" >nul || exit /b 1
echo [deploy] fakerFG.dll -> nvngx_dlssg.dll
copy /y "%SRC%fakerFG.dll" "%GAME%\nvngx_dlssg.dll" >nul || exit /b 1
copy /y "%SRC%fakerFG.dll" "%GAME%\fakerFG.dll" >nul || exit /b 1
if exist "%SRC%xess_mfg_producer.dll" (
  echo [deploy] xess_mfg_producer.dll
  copy /y "%SRC%xess_mfg_producer.dll" "%GAME%\" >nul || exit /b 1
)

rem --- 4. fakerFG.ini：只在缺失时放模板，不覆盖用户配置 ---------------------
if exist "%GAME%\fakerFG.ini" goto ini_done
if not exist "%SRC%fakerFG.ini" goto ini_done
echo [deploy] fakerFG.ini template
copy /y "%SRC%fakerFG.ini" "%GAME%\" >nul
goto ini_done
:ini_done

rem --- 5. 冲突提示 ----------------------------------------------------------
if exist "%GAME%\version.dll" (
  echo [deploy] WARNING: version.dll present (dlssg_sm86). Remove it:
  echo [deploy]          it replaces nvngx_dlssg.dll as well.
)

echo [deploy] done. Start the game, then run lock_frame_ui.exe from the package.
