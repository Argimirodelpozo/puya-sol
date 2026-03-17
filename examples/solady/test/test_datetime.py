"""Tests for Solady DateTimeLib compiled to AVM via puya-sol.

Adapted from https://github.com/Vectorized/solady/blob/main/test/DateTimeLib.t.sol
"""
import pytest
from conftest import deploy_contract, make_caller


@pytest.fixture(scope="module")
def call(algod_client, account):
    app_id, spec = deploy_contract(algod_client, account, "DateTimeWrapper", extra_pages=1)
    return make_caller(algod_client, account, app_id, spec)


class TestDateToEpochDay:
    def test_unix_epoch(self, call):
        assert call("dateToEpochDay", 1970, 1, 1) == 0

    def test_day_two(self, call):
        assert call("dateToEpochDay", 1970, 1, 2) == 1

    def test_feb_1970(self, call):
        assert call("dateToEpochDay", 1970, 2, 1) == 31

    def test_mar_1970(self, call):
        assert call("dateToEpochDay", 1970, 3, 1) == 59

    def test_apr_1970(self, call):
        assert call("dateToEpochDay", 1970, 4, 1) == 90

    def test_may_1970(self, call):
        assert call("dateToEpochDay", 1970, 5, 1) == 120

    def test_jun_1970(self, call):
        assert call("dateToEpochDay", 1970, 6, 1) == 151

    def test_jul_1970(self, call):
        assert call("dateToEpochDay", 1970, 7, 1) == 181

    def test_aug_1970(self, call):
        assert call("dateToEpochDay", 1970, 8, 1) == 212

    def test_sep_1970(self, call):
        assert call("dateToEpochDay", 1970, 9, 1) == 243

    def test_oct_1970(self, call):
        assert call("dateToEpochDay", 1970, 10, 1) == 273

    def test_nov_1970(self, call):
        assert call("dateToEpochDay", 1970, 11, 1) == 304

    def test_dec_1970(self, call):
        assert call("dateToEpochDay", 1970, 12, 1) == 334

    def test_end_1970(self, call):
        assert call("dateToEpochDay", 1970, 12, 31) == 364

    def test_start_1971(self, call):
        assert call("dateToEpochDay", 1971, 1, 1) == 365

    def test_nov_1980(self, call):
        assert call("dateToEpochDay", 1980, 11, 3) == 3959

    def test_mar_2000(self, call):
        assert call("dateToEpochDay", 2000, 3, 1) == 11017

    def test_end_2355(self, call):
        assert call("dateToEpochDay", 2355, 12, 31) == 140982


class TestEpochDayToDate:
    def test_day_zero(self, call):
        assert call("epochDayToDate", 0) == [1970, 1, 1]

    def test_day_31(self, call):
        assert call("epochDayToDate", 31) == [1970, 2, 1]

    def test_day_59(self, call):
        assert call("epochDayToDate", 59) == [1970, 3, 1]

    def test_day_365(self, call):
        assert call("epochDayToDate", 365) == [1971, 1, 1]

    def test_jan_31_2000(self, call):
        assert call("epochDayToDate", 10987) == [2000, 1, 31]

    def test_feb_29_2020(self, call):
        assert call("epochDayToDate", 18321) == [2020, 2, 29]


class TestIsLeapYear:
    def test_2000(self, call):
        assert call("isLeapYear", 2000) is True

    def test_2024(self, call):
        assert call("isLeapYear", 2024) is True

    def test_1900(self, call):
        assert call("isLeapYear", 1900) is False

    def test_2023(self, call):
        assert call("isLeapYear", 2023) is False

    def test_2100(self, call):
        assert call("isLeapYear", 2100) is False


class TestDaysInMonth:
    def test_jan(self, call):
        assert call("daysInMonth", 2022, 1) == 31

    def test_feb_non_leap(self, call):
        assert call("daysInMonth", 2022, 2) == 28

    def test_feb_leap(self, call):
        assert call("daysInMonth", 2024, 2) == 29

    def test_mar(self, call):
        assert call("daysInMonth", 2022, 3) == 31

    def test_apr(self, call):
        assert call("daysInMonth", 2022, 4) == 30

    def test_may(self, call):
        assert call("daysInMonth", 2022, 5) == 31

    def test_jun(self, call):
        assert call("daysInMonth", 2022, 6) == 30

    def test_jul(self, call):
        assert call("daysInMonth", 2022, 7) == 31

    def test_aug(self, call):
        assert call("daysInMonth", 2022, 8) == 31

    def test_sep(self, call):
        assert call("daysInMonth", 2022, 9) == 30

    def test_oct(self, call):
        assert call("daysInMonth", 2022, 10) == 31

    def test_nov(self, call):
        assert call("daysInMonth", 2022, 11) == 30

    def test_dec(self, call):
        assert call("daysInMonth", 2022, 12) == 31

    def test_feb_1900(self, call):
        assert call("daysInMonth", 1900, 2) == 28


class TestWeekday:
    def test_thu_epoch(self, call):
        # 1970-01-01 was Thursday = 4
        assert call("weekday", 1) == 4

    def test_fri(self, call):
        assert call("weekday", 86400) == 5

    def test_sat(self, call):
        assert call("weekday", 172800) == 6

    def test_sun(self, call):
        assert call("weekday", 259200) == 7

    def test_mon(self, call):
        assert call("weekday", 345600) == 1

    def test_tue(self, call):
        assert call("weekday", 432000) == 2

    def test_wed(self, call):
        assert call("weekday", 518400) == 3


class TestDateToTimestamp:
    def test_epoch(self, call):
        assert call("dateToTimestamp", 1970, 1, 1) == 0

    def test_day_two(self, call):
        assert call("dateToTimestamp", 1970, 1, 2) == 86400


class TestTimestampToDate:
    @pytest.mark.xfail(reason="opcode budget exceeded — timestampToDate needs >2000 cost")
    def test_epoch(self, call):
        assert call("timestampToDate", 0) == [1970, 1, 1]

    @pytest.mark.xfail(reason="opcode budget exceeded — timestampToDate needs >2000 cost")
    def test_day_two(self, call):
        assert call("timestampToDate", 86400) == [1970, 1, 2]
