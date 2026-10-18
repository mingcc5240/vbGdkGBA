VERSION 5.00
Begin VB.Form Form1 
   Caption         =   "vbGDkGBA -made by min-"
   ClientHeight    =   9915
   ClientLeft      =   165
   ClientTop       =   810
   ClientWidth     =   11700
   LinkTopic       =   "Form1"
   ScaleHeight     =   9915
   ScaleWidth      =   11700
   StartUpPosition =   3  'Windows 기본값
   Begin VB.Label Label1 
      Caption         =   "Label1"
      Height          =   375
      Left            =   8760
      TabIndex        =   0
      Top             =   120
      Width           =   2895
   End
   Begin VB.Menu mFile 
      Caption         =   "File"
      Begin VB.Menu mLoad 
         Caption         =   "Load"
      End
      Begin VB.Menu mExit 
         Caption         =   "Exit"
      End
   End
   Begin VB.Menu mFilter 
      Caption         =   "Filter"
      Begin VB.Menu mOrigin 
         Caption         =   "Original"
      End
      Begin VB.Menu mScale3X 
         Caption         =   "Scale3X"
      End
      Begin VB.Menu mEagle 
         Caption         =   "Scale3X/Eagle"
      End
   End
End
Attribute VB_Name = "Form1"
Attribute VB_GlobalNameSpace = False
Attribute VB_Creatable = False
Attribute VB_PredeclaredId = True
Attribute VB_Exposed = False
Option Explicit

Private m_hMemDC As Long
Private m_hBitmap As Long
Private m_hOldBitmap As Long
Private m_pBits As Long

Private m_bRunning As Boolean
Private m_bAppQuit As Boolean   ' 프로그램 종료 감지 플래그
Private m_KeyMask As Integer

' --- 고정밀 타이머 변수 ---
Private m_Frequency As Currency
Private m_TargetFrameTime As Double ' 1프레임당 시간 (초 단위: 1/59.7275 약 0.01674초)
Private m_bLoopActive As Boolean ' 루프 중복 실행 방지 플래그 (폼 상단 선언부에 추가!)

' [메뉴] 상태 저장하기 (Save State)
Private Sub mnuSaveState_Click()
    Dim SavePath As String
    SavePath = GetStateFilePath(CurrentRomPath)
    If Len(SavePath) = 0 Then Exit Sub
    
    If GBA_SaveState(SavePath) = 1 Then
        Me.Caption = "VBgdkGBA | State Saved! (" & Dir$(SavePath) & ")"
    Else
        MsgBox "상태 저장에 실패했습니다.", vbExclamation
    End If
End Sub

' [메뉴] 상태 불러오기 (Load State)
Private Sub mnuLoadState_Click()
    Dim SavePath As String
    SavePath = GetStateFilePath(CurrentRomPath)
    If Len(SavePath) = 0 Then Exit Sub
    
    If Dir$(SavePath) = "" Then
        MsgBox "저장된 세이브 파일이 없습니다: " & SavePath, vbInformation
        Exit Sub
    End If
    
    If GBA_LoadState(SavePath) = 1 Then
        DrawFrame ' 복원된 화면 즉시 그리기
        Me.Caption = "gdkGBA | State Loaded! (" & Dir$(SavePath) & ")"
    Else
        MsgBox "세이브 파일을 불러올 수 없습니다.", vbExclamation
    End If
End Sub


Private Sub Form_Load()
    Me.ScaleMode = vbPixels
    Me.AutoRedraw = True
    Me.KeyPreview = True
    
    ' Form_Load 마지막에 추가하여 게임 화면 영역을 정확히 720x480 픽셀로 고정!
Dim TopBarHeight As Long
TopBarHeight = 28 ' 상단 상태표시줄 높이
Me.Width = (720 * Screen.TwipsPerPixelX) + (Me.Width - Me.ScaleWidth * Screen.TwipsPerPixelX)
Me.Height = ((480 + TopBarHeight) * Screen.TwipsPerPixelY) + (Me.Height - Me.ScaleHeight * Screen.TwipsPerPixelY)

    
    'Me.Width = (GBA_WIDTH * 3 + 20) * Screen.TwipsPerPixelX
    'Me.Height = (GBA_HEIGHT * 3 + 90) * Screen.TwipsPerPixelY
    Me.Caption = "VBgdkGBA Player (대기 중)"

    m_KeyMask = &H3FF
    m_bRunning = False
    m_bAppQuit = False

    ' 고성능 타이머 주파수 확인
    Dim freq As Currency
    If QueryPerformanceFrequency(freq) <> 0 Then
        m_Frequency = freq
    Else
        m_Frequency = 1
    End If

    ' GBA 하드웨어 수직 주파수: 59.7275Hz (약 16.74ms)
    m_TargetFrameTime = 1# / 59.7275

    'InitDIBSection
    
    ' 480x320 DIBSection 및 메모리 DC 생성
    InitVideo3X Me.hdc
    
    
    ' 폼을 화면에 먼저 노출시킨 후 게임 루프 진입
    Me.Show
    m_bLoopActive = False
End Sub

' 메인 에뮬레이션 루프 안에서 화면 그리는 부분
Private Sub DrawFrame()
    Dim TopBarHeight As Long
    TopBarHeight = 28 ' 상단 상태표시줄 영역 높이(픽셀) - 필요에 맞게 조절하세요
    
    ' 폼 크기에 맞춰 480x320 Scale2x 화면을 StretchBlt로 꽉 차게 그림!
    RenderScreenStretchBlt Me.hdc, _
                           0, TopBarHeight, _
                           Me.ScaleWidth, Me.ScaleHeight - TopBarHeight
End Sub

Private Sub InitDIBSection()
    Dim bmi As BITMAPINFO
    
    With bmi.bmiHeader
        .biSize = Len(bmi.bmiHeader)
        .biWidth = GBA_WIDTH
        .biHeight = -GBA_HEIGHT
        .biPlanes = 1
        .biBitCount = 32
        .biCompression = 0
        .biSizeImage = GBA_WIDTH& * GBA_HEIGHT& * 4&
    End With

    m_hMemDC = CreateCompatibleDC(Me.hdc)
    m_hBitmap = CreateDIBSection(m_hMemDC, bmi, DIB_RGB_COLORS, m_pBits, 0, 0)
    m_hOldBitmap = SelectObject(m_hMemDC, m_hBitmap)
End Sub

' -----------------------------------------------------------------------------
' Timer를 쓰지 않는 60FPS 메인 루프 (Busy-Wait + 정밀 보정)
' -----------------------------------------------------------------------------
Private Sub MainGameLoop()
    Dim curTime As Currency, lastTime As Currency, fpsLastTime As Currency
    Dim elapsed As Double
    Dim fpsCount As Long
    Dim doEventsCounter As Long

    Call QueryPerformanceCounter(lastTime)
    fpsLastTime = lastTime
    fpsCount = 0
    doEventsCounter = 0

    ' 확대 모드는 루프 밖에서 한 번만 설정
    Call SetStretchBltMode(Me.hdc, COLORONCOLOR)

    Do While Not m_bAppQuit
        If m_bRunning Then
            Call QueryPerformanceCounter(curTime)
            elapsed = (curTime - lastTime) / m_Frequency

            ' 목표 프레임 간격 (약 0.0167초)
            If elapsed >= m_TargetFrameTime Then
                lastTime = curTime

                ' 1. 키 입력 전달
                Call GBA_SetKeys(m_KeyMask)

                ' 2. 에뮬레이션 연산
                Dim pFrameBuffer As Long
                pFrameBuffer = GBA_RunFrame()

                ' 3. 버퍼 전송 (DLL 버퍼 -> DIBSection)
                If pFrameBuffer <> 0 And m_pBits <> 0 Then
                    Call CopyMemory(ByVal m_pBits, ByVal pFrameBuffer, GBA_FRAME_BYTES)
                End If

                ' 4. 화면 출력 (Refresh 호출 제거)
                Call StretchBlt(Me.hdc, 0, 35, GBA_WIDTH * 3, GBA_HEIGHT * 3, _
                                m_hMemDC, 0, 0, GBA_WIDTH * 2, GBA_HEIGHT * 2, SRCCOPY)

                fpsCount = fpsCount + 1

                ' 2프레임마다 1번만 DoEvents를 호출하여 CPU 낭비 제거
                doEventsCounter = doEventsCounter + 1
                If (doEventsCounter And 1) = 0 Then
                    DoEvents
                End If
            Else
                ' 여유 시간이 2.5ms 이상 남아있을 때만 Sleep
                If (m_TargetFrameTime - elapsed) > 0.0025 Then
                    Call Sleep(1)
                End If
            End If

            ' 1초마다 FPS 갱신
            Call QueryPerformanceCounter(curTime)
            If ((curTime - fpsLastTime) / m_Frequency) >= 1# Then
                Me.Caption = "gdkGBA | FPS: " & fpsCount
                fpsCount = 0
                fpsLastTime = curTime
            End If
        Else
            Call Sleep(10)
            DoEvents
        End If
    Loop
End Sub

Private Sub MainGameLoop0()
    Dim curTime As Currency, lastTime As Currency, fpsLastTime As Currency
    Dim elapsed As Double
    Dim fpsCount As Long
    Dim romTitle As String
    Dim lcount As Long
    

    Call QueryPerformanceCounter(lastTime)
    fpsLastTime = lastTime
    fpsCount = 0

    lcount = 0
    Do While Not m_bAppQuit
        ' 1. 게임이 구동 중일 때 프레임 시간 도달 여부 확인
        If m_bRunning Then
            Call QueryPerformanceCounter(curTime)
            elapsed = (curTime - lastTime) / m_Frequency

            ' 1프레임 목표 시간 도달 시 실행
            If elapsed >= m_TargetFrameTime Then
                lastTime = curTime

                ' [1] 키 입력 전달
                Call GBA_SetKeys(m_KeyMask)

                ' [2] 프레임 연산 및 32비트 버퍼 복사
                Dim pFrameBuffer As Long
                pFrameBuffer = GBA_RunFrame()
               
                 
                If pFrameBuffer <> 0 And m_pBits <> 0 Then
                    Call CopyMemory(ByVal m_pBits, ByVal pFrameBuffer, GBA_FRAME_BYTES)
                End If

               
                'Call SetStretchBltMode(Me.hDC, COLORONCOLOR)
                'Call StretchBlt(Me.hDC, 0, 35, 480, 320, m_hMemDC, 0, 0, 480, 320, SRCCOPY)
                 Call DrawFrame
                 
                 Call Sound_Update
                Me.Refresh
                ' 4. [신규 추가] 오디오 큐 갱신 (화면 프레임과 1:1 동기화)
               
                
                ' [4] FPS 카운트 누적
                fpsCount = fpsCount + 1
           ' Else
                ' 다음 프레임까지 1ms 이상 남았으면 CPU 점유율 과열 방지를 위해 잠시 양보
           '     If (m_TargetFrameTime - elapsed) > 0.002 Then
           '         Call Sleep(1)
            '    End If
            End If

            ' 1초(1.0초)마다 FPS 계산 및 타이틀 표시
            Call QueryPerformanceCounter(curTime)
            If ((curTime - fpsLastTime) / m_Frequency) >= 1# Then
                Me.Caption = "gdkGBA | FPS: " & fpsCount
                Label1.Caption = Sound_GetDebugInfo()
                fpsCount = 0
                fpsLastTime = curTime
            End If
        Else
            ' 대기 중일 때는 CPU 점유율을 낮춤
            Call Sleep(1)
        End If

        ' 2. 윈도우 UI 및 키보드 이벤트 반응 처리
       ' DoEvents
        lcount = (lcount + 1) And &HFFFF&
        If (lcount Mod 2) Then DoEvents
    Loop
End Sub

' -----------------------------------------------------------------------------
' 키보드 입력 매핑
' -----------------------------------------------------------------------------
Private Sub Form_KeyDown(KeyCode As Integer, Shift As Integer)
    If KeyCode = vbKeyF1 Then
        mnuSaveState_Click
    ElseIf KeyCode = vbKeyF2 Then
        mnuLoadState_Click
    End If
  
    Select Case KeyCode
        Case vbKeyZ:      m_KeyMask = m_KeyMask And Not 1    ' A
        Case vbKeyX:      m_KeyMask = m_KeyMask And Not 2    ' B
        Case vbKeyShift:  m_KeyMask = m_KeyMask And Not 4    ' Select
        Case vbKeyReturn: m_KeyMask = m_KeyMask And Not 8    ' Start
        Case vbKeyRight:  m_KeyMask = m_KeyMask And Not 16   ' Right
        Case vbKeyLeft:   m_KeyMask = m_KeyMask And Not 32   ' Left
        Case vbKeyUp:     m_KeyMask = m_KeyMask And Not 64   ' Up
        Case vbKeyDown:   m_KeyMask = m_KeyMask And Not 128  ' Down
        Case vbKeyS:      m_KeyMask = m_KeyMask And Not 256  ' R
        Case vbKeyA:      m_KeyMask = m_KeyMask And Not 512  ' L
    End Select
    
  
End Sub

Private Sub Form_KeyUp(KeyCode As Integer, Shift As Integer)
   
    Select Case KeyCode
        Case vbKeyZ:      m_KeyMask = m_KeyMask Or 1
        Case vbKeyX:      m_KeyMask = m_KeyMask Or 2
        Case vbKeyShift:  m_KeyMask = m_KeyMask Or 4
        Case vbKeyReturn: m_KeyMask = m_KeyMask Or 8
        Case vbKeyRight:  m_KeyMask = m_KeyMask Or 16
        Case vbKeyLeft:   m_KeyMask = m_KeyMask Or 32
        Case vbKeyUp:     m_KeyMask = m_KeyMask Or 64
        Case vbKeyDown:   m_KeyMask = m_KeyMask Or 128
        Case vbKeyS:      m_KeyMask = m_KeyMask Or 256
        Case vbKeyA:      m_KeyMask = m_KeyMask Or 512
    End Select
End Sub

Private Sub Form_QueryUnload(Cancel As Integer, UnloadMode As Integer)
    ' 무한 루프 탈출 신호 설정
    m_bAppQuit = True
    m_bRunning = False
End Sub

Private Sub Form_Unload(Cancel As Integer)
    Call Sound_Close
    Call GBA_Close

    If m_hOldBitmap <> 0 Then Call SelectObject(m_hMemDC, m_hOldBitmap)
    If m_hBitmap <> 0 Then Call DeleteObject(m_hBitmap)
    If m_hMemDC <> 0 Then Call DeleteDC(m_hMemDC)
End Sub



' -----------------------------------------------------------------------------
' 롬 로더 및 리셋 함수
' -----------------------------------------------------------------------------
Public Function LoadNewROM1(ByVal sRomPath As String) As Boolean
    LoadNewROM1 = False
    
    ' 1. 먼저 실행 중인 게임 루프 동작을 일시 정지
    m_bRunning = False

    ' 2. 이전 사운드 및 이전 에뮬레이터 메모리 완전히 닫기 (매우 중요!)
    Call Sound_Close
    Call GBA_Close   ' <-- GBA_Reset 대신 이전 ROM 리소스를 먼저 해제해야 합니다!
    
    ' 3. 화면 버퍼를 검은색으로 깨끗이 지움
    If m_pBits <> 0 Then
        Call ZeroMemory(m_pBits, GBA_FRAME_BYTES)
    End If
    
    Call SetStretchBltMode(Me.hdc, COLORONCOLOR)
    Call StretchBlt(Me.hdc, 0, 35, GBA_WIDTH * 3, GBA_HEIGHT * 3, _
                    m_hMemDC, 0, 0, GBA_WIDTH, GBA_HEIGHT, SRCCOPY)
    Me.Refresh

    m_KeyMask = &H3FF

    ' 4. 새 ROM 로드 및 하드웨어 리셋
    If GBA_Init(sRomPath) = 0 Then
        Call GBA_Reset ' <-- Init 이후에 Reset을 불러야 새 ROM의 시작 주소(0x08000000)로 세팅됩니다!
        
        If Not Sound_Init() Then
            MsgBox "오디오 출력 장치(waveOut)를 초기화하지 못했습니다.", vbExclamation
        End If
        
        m_bRunning = True
        LoadNewROM1 = True
    Else
        MsgBox "새로운 ROM을 초기화하지 못했습니다.", vbCritical, "오류"
        Me.Caption = "gdkGBA Player (대기 중)"
    End If
End Function

Public Function LoadNewROM(ByVal sRomPath As String) As Boolean

    LoadNewROM = False

    m_bRunning = False



    Call GBA_Reset

    Call Sound_Close ' 이전 사운드 버퍼 정리

    

    If m_pBits <> 0 Then

        Call ZeroMemory(m_pBits, GBA_FRAME_BYTES)

    End If

    
    Call SetStretchBltMode(Me.hdc, COLORONCOLOR)

    Call StretchBlt(Me.hdc, 0, 35, GBA_WIDTH * 3, GBA_HEIGHT * 3, m_hMemDC, 0, 0, GBA_WIDTH * 2, GBA_HEIGHT * 2, SRCCOPY)

    Me.Refresh



    m_KeyMask = &H3FF



    If GBA_Init(sRomPath) = 0 Then

        m_bRunning = True

        LoadNewROM = True

      

        If Not Sound_Init() Then

            MsgBox "오디오 출력 장치(waveOut)를 초기화하지 못했습니다.", vbExclamation

        End If

    Else

        MsgBox "새로운 ROM을 초기화하지 못했습니다.", vbCritical, "오류"

        Me.Caption = "gdkGBA Player (대기 중)"

    End If
   
   CurrentRomPath = sRomPath
  ' MsgBox CurrentRomPath
End Function

Private Sub mEagle_Click()
 ' 1. 메뉴 체크 표시 업데이트
    mOrigin.Checked = False
    mScale3X.Checked = False
    mEagle.Checked = True
    
    ' 2. C DLL을 원본 도트 모드(0)로 변경
    GBA_SetVideoFilter 2
    
    ' 3. 현재 화면 즉시 다시 그리기
    DrawFrame
End Sub

Private Sub mExit_Click()
    Call Sound_Close
    Call GBA_Close

    If m_hOldBitmap <> 0 Then Call SelectObject(m_hMemDC, m_hOldBitmap)
    If m_hBitmap <> 0 Then Call DeleteObject(m_hBitmap)
    If m_hMemDC <> 0 Then Call DeleteDC(m_hMemDC)
 End
End Sub


Private Sub mLoad_Click()
   Dim RomPath As String
    RomPath = ShowOpenFileDialog(Me.hWnd)
    If Trim$(RomPath) = "" Then Exit Sub

    Call LoadNewROM(RomPath)
    
    Call MainGameLoop0 ' <-- 두 번째 불러올 때 루프 안에 또 루프가 생성됨!
End Sub

Private Sub mOrigin_Click()
 ' 1. 메뉴 체크 표시 업데이트
    mOrigin.Checked = True
    mScale3X.Checked = False
    mEagle.Checked = False
    
    ' 2. C DLL을 원본 도트 모드(0)로 변경
    GBA_SetVideoFilter 0
    
    ' 3. 현재 화면 즉시 다시 그리기
    DrawFrame
End Sub

Private Sub mScale3X_Click()
  ' 1. 메뉴 체크 표시 업데이트
    mOrigin.Checked = False
    mScale3X.Checked = True
    mEagle.Checked = False
    
    ' 2. C DLL을 원본 도트 모드(0)로 변경
    GBA_SetVideoFilter 1
    
    ' 3. 현재 화면 즉시 다시 그리기
    DrawFrame
End Sub
