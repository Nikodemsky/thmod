@if exist personal.props goto NEXT
@copy tools\personal.props personal.props
:NEXT
@if exist thehell3.vcxproj.user goto END
@copy tools\thehell3.vcxproj.user thehell3.vcxproj.user
:END
