VERSION 5.00
Begin VB.Form Form1 
   Caption         =   "mGBA with VB6.0"
   ClientHeight    =   9540
   ClientLeft      =   60
   ClientTop       =   405
   ClientWidth     =   15060
   LinkTopic       =   "Form1"
   ScaleHeight     =   636
   ScaleMode       =   3  '픽셀
   ScaleWidth      =   1004
   StartUpPosition =   3  'Windows 기본값
   Begin VB.Timer tmrCopy 
      Enabled         =   0   'False
      Interval        =   16
      Left            =   10920
      Top             =   7560
   End
   Begin VB.PictureBox PicClone 
      Height          =   4800
      Left            =   7560
      ScaleHeight     =   316
      ScaleMode       =   3  '픽셀
      ScaleWidth      =   476
      TabIndex        =   2
      Top             =   120
      Width           =   7200
   End
   Begin VB.Timer tmrFindWindow 
      Enabled         =   0   'False
      Interval        =   100
      Left            =   7560
      Top             =   8280
   End
   Begin VB.CommandButton cmdLaunch 
      Caption         =   "mGBA Launch "
      Height          =   495
      Left            =   600
      TabIndex        =   1
      Top             =   8040
      Width           =   2415
   End
   Begin VB.PictureBox PicScreen 
      Height          =   4800
      Left            =   120
      ScaleHeight     =   4740
      ScaleWidth      =   7140
      TabIndex        =   0
      Top             =   120
      Width           =   7200
   End
End
Attribute VB_Name = "Form1"
Attribute VB_GlobalNameSpace = False
Attribute VB_Creatable = False
Attribute VB_PredeclaredId = True
Attribute VB_Exposed = False
Option Explicit

Private Const ROM_FILENAME As String = "metalslug.gba"
Private Const MENU_HEIGHT As Long = 21

' 원본 해상도(240x160) 및 Edge2X 확대 해상도(480x320)
Private Const GBA_W As Long = 240
Private Const GBA_H As Long = 160
Private Const DST_W As Long = 480 ' 240 * 2
Private Const DST_H As Long = 320 ' 160 * 2

' 순수 VB6 1차원 픽셀 배열 2개
Private m_SrcPixels() As Long ' 0 To 38,399  (240 x 160)
Private m_DstPixels() As Long ' 0 To 153,599 (480 x 320)

Private m_SrcBMI As BITMAPINFO
Private m_DstBMI As BITMAPINFO
Private m_CapBuf As DIBBuffer

' 필터 모드 (0: 기본 확대, 1: 순수 VB6 Edge2X, 2: Edge2X + 스캔라인)
Private m_FilterMode As Integer

Private Function GetAppPath(ByVal sFileName As String) As String
    If Right$(App.Path, 1) = "\" Then
        GetAppPath = App.Path & sFileName
    Else
        GetAppPath = App.Path & "\" & sFileName
    End If
End Function

Private Sub Form_Load()
    Me.KeyPreview = True
    
    PicScreen.ScaleMode = vbPixels
    PicScreen.BackColor = vbBlack
    
    PicClone.ScaleMode = vbPixels
    PicClone.BackColor = vbBlack
    PicClone.AutoRedraw = False
    SetStretchBltMode PicClone.hDC, COLORONCOLOR
    
    ' 1. 캡처용 DIB 버퍼 생성 (240x160)
    m_CapBuf = CreateFastDIB(GBA_W, GBA_H)
    
    ' 2. 원본(240x160) 및 타겟(480x320) 1차원 배열 메모리 할당
    ReDim m_SrcPixels(0 To (GBA_W * GBA_H) - 1) As Long
    ReDim m_DstPixels(0 To (DST_W * DST_H) - 1) As Long
    
    ' 3. 원본 배열용 BITMAPINFO (240x160)
    With m_SrcBMI.bmiHeader
        .biSize = Len(m_SrcBMI.bmiHeader)
        .biWidth = GBA_W
        .biHeight = -GBA_H
        .biPlanes = 1
        .biBitCount = 32
        .biCompression = BI_RGB
    End With
    
    ' 4. 타겟 배열용 BITMAPINFO (480x320)
    With m_DstBMI.bmiHeader
        .biSize = Len(m_DstBMI.bmiHeader)
        .biWidth = DST_W
        .biHeight = -DST_H
        .biPlanes = 1
        .biBitCount = 32
        .biCompression = BI_RGB
    End With
    
    m_FilterMode = 1
    Me.Caption = "mGBA with VB6.0 - [F2: 순수 VB6 Edge2X 적용 중]"
End Sub

Private Sub cmdLaunch_Click()
    Dim mgbaPath As String
    Dim romPath As String
    Dim cmdLine As String
    
    mgbaPath = GetAppPath("mGBA.exe")
    If Dir$(mgbaPath) = "" Then
        MsgBox "현재 폴더에서 mGBA.exe를 찾을 수 없습니다.", vbExclamation
        Exit Sub
    End If
    
    If ROM_FILENAME <> "" Then
        romPath = GetAppPath(ROM_FILENAME)
        If Dir$(romPath) <> "" Then
            cmdLine = """" & mgbaPath & """ """ & romPath & """"
        Else
            cmdLine = """" & mgbaPath & """"
        End If
    Else
        cmdLine = """" & mgbaPath & """"
    End If
    
    If g_mgbaHwnd <> 0 Then
        tmrCopy.Enabled = False
        PostMessage g_mgbaHwnd, WM_CLOSE, 0, 0
        g_mgbaHwnd = 0
    End If
    
    g_TargetPID = Shell(cmdLine, vbNormalFocus)
    tmrFindWindow.Interval = 100
    tmrFindWindow.Enabled = True
    PicScreen.SetFocus
End Sub

Private Sub tmrFindWindow_Timer()
    EnumWindows AddressOf EnumWindowsProc, 0
    If g_mgbaHwnd <> 0 Then
        tmrFindWindow.Enabled = False
        EmbedWindow g_mgbaHwnd
        
        tmrCopy.Interval = 16
        tmrCopy.Enabled = True
    End If
End Sub

Private Sub EmbedWindow(ByVal hWnd As Long)
    Dim lStyle As Long
    lStyle = GetWindowLong(hWnd, GWL_STYLE)
    lStyle = lStyle And Not (WS_CAPTION Or WS_THICKFRAME Or WS_POPUP)
    lStyle = lStyle Or WS_CHILD
    SetWindowLong hWnd, GWL_STYLE, lStyle
    
    SetParent hWnd, PicScreen.hWnd
    SetWindowPos hWnd, 0, 0, 0, 0, 0, SWP_NOZORDER Or SWP_NOMOVE Or SWP_NOSIZE Or SWP_FRAMECHANGED
    MoveWindow hWnd, 0, 0, PicScreen.ScaleWidth, PicScreen.ScaleHeight, 1
    
    PostMessage hWnd, WM_ACTIVATE, WA_ACTIVE, 0
    PicScreen.SetFocus
End Sub

' 매 프레임 캡처 -> 1차원 배열 추출 -> VB6 Edge2X 연산 -> picClone 출력
Private Sub tmrCopy_Timer()
    If g_mgbaHwnd = 0 Then Exit Sub
    
    Dim hSrcDC As Long
    Dim srcW As Long, srcH As Long
    
    hSrcDC = GetDC(g_mgbaHwnd)
    If hSrcDC = 0 Then Exit Sub
    
    srcW = PicScreen.ScaleWidth
    srcH = PicScreen.ScaleHeight - MENU_HEIGHT
    
    ' 1단계: mGBA 화면(메뉴바 제외)을 240x160 크기로 정규화하여 캡처
    SetStretchBltMode m_CapBuf.hDC, COLORONCOLOR
    StretchBlt m_CapBuf.hDC, 0, 0, GBA_W, GBA_H, hSrcDC, 0, MENU_HEIGHT, srcW, srcH, SRCCOPY
    ReleaseDC g_mgbaHwnd, hSrcDC
    
    ' 2단계: 캡처된 240x160 비트맵을 원본 1차원 배열 m_SrcPixels(0 ~ 38399)로 복사
    GetDIBits m_CapBuf.hDC, m_CapBuf.hDIB, 0, GBA_H, m_SrcPixels(0), m_SrcBMI, DIB_RGB_COLORS
    
    ' 3단계: 필터 모드에 따라 처리 후 picClone에 출력
    If m_FilterMode = 0 Then
        ' [F1 모드] 원본 240x160 배열을 그대로 출력 (네모난 도트 유지)
        StretchDIBits PicClone.hDC, 0, 0, PicClone.ScaleWidth, PicClone.ScaleHeight, _
                      0, 0, GBA_W, GBA_H, m_SrcPixels(0), m_SrcBMI, DIB_RGB_COLORS, SRCCOPY
    Else
        ' [F2 / F3 모드] 순수 VB6 코드로 240x160 배열을 480x320 배열로 Edge2X 업스케일링!
        ApplyEdge2X_VB6 (m_FilterMode = 2)
        
        ' 완성된 480x320 타겟 배열(m_DstPixels)을 picClone에 출력
        StretchDIBits PicClone.hDC, 0, 0, PicClone.ScaleWidth, PicClone.ScaleHeight, _
                      0, 0, DST_W, DST_H, m_DstPixels(0), m_DstBMI, DIB_RGB_COLORS, SRCCOPY
    End If
End Sub

' =============================================================================
' [핵심 교육용 알고리즘] 순수 VB6 1차원 배열 Edge2X (Scale2x) 구현
' 입력: m_SrcPixels(0 To 38399)  [240 x 160]
' 출력: m_DstPixels(0 To 153599) [480 x 320]
' =============================================================================
Private Sub ApplyEdge2X_VB6(ByVal UseScanline As Boolean)
    Dim x As Long, y As Long
    Dim rowPrev As Long, rowCurr As Long, rowNext As Long
    Dim dstRow0 As Long, dstRow1 As Long
    Dim xPrev As Long, xNext As Long
    Dim dx As Long
    
    ' 주변 픽셀(B, D, E, F, H) 및 2x2 확장 픽셀(E0, E1, E2, E3) 색상 변수
    Dim B As Long, D As Long, E As Long, F As Long, H As Long
    Dim E0 As Long, E1 As Long, E2 As Long, E3 As Long
    
    For y = 0 To GBA_H - 1
        ' 1. 1차원 배열에서 현재 줄(y), 윗줄(y-1), 아랫줄(y+1)의 시작 인덱스 계산
        rowCurr = y * GBA_W
        If y > 0 Then rowPrev = rowCurr - GBA_W Else rowPrev = 0
        If y < GBA_H - 1 Then rowNext = rowCurr + GBA_W Else rowNext = rowCurr
        
        ' 2. 480x320 타겟 1차원 배열에서 짝수 줄(y*2)과 홀수 줄(y*2+1)의 시작 인덱스 계산
        dstRow0 = (y * 2) * DST_W
        dstRow1 = dstRow0 + DST_W
        
        For x = 0 To GBA_W - 1
            ' 좌우 경계 인덱스 계산
            If x > 0 Then xPrev = x - 1 Else xPrev = 0
            If x < GBA_W - 1 Then xNext = x + 1 Else xNext = GBA_W - 1
            
            ' 3. 1차원 배열에서 십자(+) 모양 5개 픽셀 꺼내기
            '      [B]
            '   [D][E][F]
            '      [H]
            B = m_SrcPixels(rowPrev + x)
            D = m_SrcPixels(rowCurr + xPrev)
            E = m_SrcPixels(rowCurr + x)
            F = m_SrcPixels(rowCurr + xNext)
            H = m_SrcPixels(rowNext + x)
            
            ' 4. Edge2X 대각선 보간 규칙 판별
            If (B <> H) And (D <> F) Then
                If D = B Then E0 = D Else E0 = E
                If B = F Then E1 = F Else E1 = E
                If D = H Then E2 = D Else E2 = E
                If H = F Then E3 = F Else E3 = E
            Else
                E0 = E
                E1 = E
                E2 = E
                E3 = E
            End If
            
            ' 5. 480x320 타겟 배열에 2x2 픽셀로 기록
            dx = x * 2
            m_DstPixels(dstRow0 + dx) = E0
            m_DstPixels(dstRow0 + dx + 1) = E1
            
            If UseScanline Then
                ' [F3 모드] 아랫줄(홀수 줄) 밝기를 50%로 낮춰 브라운관 스캔라인 합성
                m_DstPixels(dstRow1 + dx) = (E2 \ 2) And &H7F7F7F
                m_DstPixels(dstRow1 + dx + 1) = (E3 \ 2) And &H7F7F7F
            Else
                m_DstPixels(dstRow1 + dx) = E2
                m_DstPixels(dstRow1 + dx + 1) = E3
            End If
        Next x
    Next y
End Sub

Private Sub Form_KeyDown(KeyCode As Integer, Shift As Integer)
    ' F1, F2, F3 키로 실시간 화질 비교!
    Select Case KeyCode
        Case vbKeyF1
            m_FilterMode = 0
            Me.Caption = "mGBA with VB6.0 - [F1: 원본 도트 (240x160)]"
            Exit Sub
        Case vbKeyF2
            m_FilterMode = 1
            Me.Caption = "mGBA with VB6.0 - [F2: 순수 VB6 Edge2X (480x320)]"
            Exit Sub
        Case vbKeyF3
            m_FilterMode = 2
            Me.Caption = "mGBA with VB6.0 - [F3: 순수 VB6 Edge2X + CRT 스캔라인]"
            Exit Sub
    End Select

    If g_mgbaHwnd <> 0 Then
        PostMessage g_mgbaHwnd, WM_KEYDOWN, KeyCode, MakeKeyLParam(KeyCode, False)
        KeyCode = 0
    End If
End Sub

Private Sub Form_KeyUp(KeyCode As Integer, Shift As Integer)
    If g_mgbaHwnd <> 0 Then
        PostMessage g_mgbaHwnd, WM_KEYUP, KeyCode, MakeKeyLParam(KeyCode, True)
        KeyCode = 0
    End If
End Sub

Private Sub Form_Unload(Cancel As Integer)
    tmrCopy.Enabled = False
    If g_mgbaHwnd <> 0 Then
        PostMessage g_mgbaHwnd, WM_CLOSE, 0, 0
        g_mgbaHwnd = 0
    End If
    DestroyFastDIB m_CapBuf
End Sub

