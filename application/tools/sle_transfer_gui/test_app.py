import unittest

from app import ROLE_DEBUG, ROLE_RECEIVE, ROLE_SEND, normalize_role, role_page_keys


class RoleModeTests(unittest.TestCase):
    def test_normalize_role_keeps_supported_roles(self) -> None:
        self.assertEqual(normalize_role(ROLE_SEND), ROLE_SEND)
        self.assertEqual(normalize_role(ROLE_RECEIVE), ROLE_RECEIVE)
        self.assertEqual(normalize_role(ROLE_DEBUG), ROLE_DEBUG)

    def test_normalize_role_falls_back_to_debug(self) -> None:
        self.assertEqual(normalize_role(None), ROLE_DEBUG)
        self.assertEqual(normalize_role("unknown"), ROLE_DEBUG)

    def test_role_page_keys(self) -> None:
        self.assertEqual(role_page_keys(ROLE_SEND), (ROLE_SEND,))
        self.assertEqual(role_page_keys(ROLE_RECEIVE), (ROLE_RECEIVE,))
        self.assertEqual(role_page_keys(ROLE_DEBUG), (ROLE_SEND, ROLE_RECEIVE))


if __name__ == "__main__":
    unittest.main()
