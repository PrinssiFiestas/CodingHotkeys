#NoEnv
#SingleInstance force
#Persistent
#Warn

; Interpret Ctrl+Alt as AltGr when using cmd.exe
; Only tested with Finnish keyboard!

#IfWinActive ahk_class ConsoleWindowClass
<^<!7::SendInput {RAlt DOWN}7{RAlt UP}
<^<!8::SendInput {RAlt DOWN}8{RAlt UP}
<^<!9::SendInput {RAlt DOWN}9{RAlt UP}
<^<!0::SendInput {RAlt DOWN}0{RAlt UP}
<^<!VKBB::SendInput {RAlt DOWN}{VKBB}{RAlt UP}
<^<SC1B::SendInput ~
#IfWinActive
