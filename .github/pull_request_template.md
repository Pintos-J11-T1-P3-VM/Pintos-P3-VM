## 변경 사항

### 구현 내용
<!-- 구현내용에 대해서 설명해주세요. -->
<!-- 예시:
- Priority Scheduling 구현: ready_list를 우선순위 기반으로 관리 
-->

### 변경된 파일
<!-- 구현 내용으로 인해 변경된 파일에 대해 설명해주세요 -->
<!-- 예시:
- `threads/thread.c`
  - 코드 컨벤션에 맞춰 포매팅 적용
  - `cmp_thread_priority()` 함수를 전역 함수로 변경 (세마포어에서 사용)
  - `thread_unblock()`: 우선순위 순으로 ready_list에 삽입
  - `thread_yield()`: 우선순위 순으로 ready_list에 삽입
  - `thread_create()`: 생성된 스레드가 현재 스레드보다 높은 우선순위면 즉시 선점
  - `thread_set_priority()`: `original_priority` 갱신 후 `refresh_priority()` 호출, ready_list 검사 후 필요시 양보
  - `refresh_priority()` 함수 추가: donation 리스트에서 최고 우선순위로 현재 우선순위 갱신
 -->

## 체크리스트
<!-- 체크리스트를 모두 완료한 후 제출해주세요 -->
<!-- 완료 처리를 표시하려면 [x], 완료되지 않았다면 [ ] -->
- [ ] 코드 스타일 가이드 준수
- [ ] 불필요한 주석/디버깅 코드 제거
- [ ] 커밋 메시지 컨벤션 준수
- [ ] 관련 테스트 통과 확인

## 추가 설명
<!-- 추가로 설명할 내용이 있으시다면 아래에 작성해주세요. -->