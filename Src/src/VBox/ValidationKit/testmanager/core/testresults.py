# -*- coding: utf-8 -*-
# $Id: testresults.py $
# pylint: disable=C0302

## @todo Rename this file to testresult.py!

"""
Test Manager - Fetch test results.
"""

__copyright__ = \
"""
Copyright (C) 2012-2016 Oracle Corporation

This file is part of VirtualBox Open Source Edition (OSE), as
available from http://www.virtualbox.org. This file is free software;
you can redistribute it and/or modify it under the terms of the GNU
General Public License (GPL) as published by the Free Software
Foundation, in version 2 as it comes in the "COPYING" file of the
VirtualBox OSE distribution. VirtualBox OSE is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.

The contents of this file may alternatively be used under the terms
of the Common Development and Distribution License Version 1.0
(CDDL) only, as it comes in the "COPYING.CDDL" file of the
VirtualBox OSE distribution, in which case the provisions of the
CDDL are applicable instead of those of the GPL.

You may elect to license modified versions of this file under the
terms and conditions of either the GPL or the CDDL or both.
"""
__version__ = "$Revision: 109040 $"
# Standard python imports.
import unittest;

# Validation Kit imports.
from common                                 import constants;
from testmanager                            import config;
from testmanager.core.base                  import ModelDataBase, ModelLogicBase, ModelDataBaseTestCase, TMExceptionBase, \
                                                   TMTooManyRows, TMRowNotFound;
from testmanager.core.testgroup             import TestGroupData;
from testmanager.core.build                 import BuildDataEx;
from testmanager.core.failurereason         import FailureReasonLogic;
from testmanager.core.testbox               import TestBoxData;
from testmanager.core.testcase              import TestCaseData;
from testmanager.core.schedgroup            import SchedGroupData;
from testmanager.core.systemlog             import SystemLogData, SystemLogLogic;
from testmanager.core.testresultfailures    import TestResultFailureDataEx;
from testmanager.core.useraccount           import UserAccountLogic;


class TestResultData(ModelDataBase):
    """
    Test case execution result data
    """

    ## @name TestStatus_T
    # @{
    ksTestStatus_Running    = 'running';
    ksTestStatus_Success    = 'success';
    ksTestStatus_Skipped    = 'skipped';
    ksTestStatus_BadTestBox = 'bad-testbox';
    ksTestStatus_Aborted    = 'aborted';
    ksTestStatus_Failure    = 'failure';
    ksTestStatus_TimedOut   = 'timed-out';
    ksTestStatus_Rebooted   = 'rebooted';
    ## @}

    ## List of relatively harmless (to testgroup/case) statuses.
    kasHarmlessTestStatuses = [ ksTestStatus_Skipped, ksTestStatus_BadTestBox, ksTestStatus_Aborted, ];
    ## List of bad statuses.
    kasBadTestStatuses      = [ ksTestStatus_Failure, ksTestStatus_TimedOut,   ksTestStatus_Rebooted, ];


    ksIdAttr    = 'idTestResult';

    ksParam_idTestResult        = 'TestResultData_idTestResult';
    ksParam_idTestResultParent  = 'TestResultData_idTestResultParent';
    ksParam_idTestSet           = 'TestResultData_idTestSet';
    ksParam_tsCreated           = 'TestResultData_tsCreated';
    ksParam_tsElapsed           = 'TestResultData_tsElapsed';
    ksParam_idStrName           = 'TestResultData_idStrName';
    ksParam_cErrors             = 'TestResultData_cErrors';
    ksParam_enmStatus           = 'TestResultData_enmStatus';
    ksParam_iNestingDepth       = 'TestResultData_iNestingDepth';
    kasValidValues_enmStatus    = [
        ksTestStatus_Running,
        ksTestStatus_Success,
        ksTestStatus_Skipped,
        ksTestStatus_BadTestBox,
        ksTestStatus_Aborted,
        ksTestStatus_Failure,
        ksTestStatus_TimedOut,
        ksTestStatus_Rebooted
    ];


    def __init__(self):
        ModelDataBase.__init__(self)
        self.idTestResult       = None
        self.idTestResultParent = None
        self.idTestSet          = None
        self.tsCreated          = None
        self.tsElapsed          = None
        self.idStrName          = None
        self.cErrors            = 0;
        self.enmStatus          = None
        self.iNestingDepth      = None

    def initFromDbRow(self, aoRow):
        """
        Reinitialize from a SELECT * FROM TestResults.
        Return self. Raises exception if no row.
        """
        if aoRow is None:
            raise TMRowNotFound('Test result record not found.')

        self.idTestResult       = aoRow[0]
        self.idTestResultParent = aoRow[1]
        self.idTestSet          = aoRow[2]
        self.tsCreated          = aoRow[3]
        self.tsElapsed          = aoRow[4]
        self.idStrName          = aoRow[5]
        self.cErrors            = aoRow[6]
        self.enmStatus          = aoRow[7]
        self.iNestingDepth      = aoRow[8]
        return self;

    def initFromDbWithId(self, oDb, idTestResult, tsNow = None, sPeriodBack = None):
        """
        Initialize from the database, given the ID of a row.
        """
        _ = tsNow;
        _ = sPeriodBack;
        oDb.execute('SELECT *\n'
                    'FROM   TestResults\n'
                    'WHERE  idTestResult = %s\n'
                    , ( idTestResult,));
        aoRow = oDb.fetchOne()
        if aoRow is None:
            raise TMRowNotFound('idTestResult=%s not found' % (idTestResult,));
        return self.initFromDbRow(aoRow);

    def isFailure(self):
        """ Check if it's a real failure. """
        return self.enmStatus in self.kasBadTestStatuses;


class TestResultDataEx(TestResultData):
    """
    Extended test result data class.

    This is intended for use as a node in a result tree.  This is not intended
    for serialization to parameters or vice versa.  Use TestResultLogic to
    construct the tree.
    """

    def __init__(self):
        TestResultData.__init__(self)
        self.sName      = None; # idStrName resolved.
        self.oParent    = None; # idTestResultParent within the tree.

        self.aoChildren = [];   # TestResultDataEx;
        self.aoValues   = [];   # TestResultValueDataEx;
        self.aoMsgs     = [];   # TestResultMsgDataEx;
        self.aoFiles    = [];   # TestResultFileDataEx;
        self.oReason    = None; # TestResultReasonDataEx;

    def initFromDbRow(self, aoRow):
        """
        Initialize from a query like this:
            SELECT TestResults.*, TestResultStrTab.sValue
            FROM TestResults, TestResultStrTab
            WHERE TestResultStrTab.idStr = TestResults.idStrName

        Note! The caller is expected to fetch children, values, failure
              details, and files.
        """
        self.sName      = None;
        self.oParent    = None;
        self.aoChildren = [];
        self.aoValues   = [];
        self.aoMsgs     = [];
        self.aoFiles    = [];
        self.oReason    = None;

        TestResultData.initFromDbRow(self, aoRow);

        self.sName = aoRow[9];
        return self;

    def deepCountErrorContributers(self):
        """
        Counts how many test result instances actually contributed to cErrors.
        """

        # Check each child (if any).
        cChanges = 0;
        cChildErrors = 0;
        for oChild in self.aoChildren:
            if oChild.cErrors > 0:
                cChildErrors += oChild.cErrors;
                cChanges     += oChild.deepCountErrorContributers();

        # Did we contribute as well?
        if self.cErrors > cChildErrors:
            cChanges += 1;
        return cChanges;

    def getListOfFailures(self):
        """
        Get a list of test results insances actually contributing to cErrors.

        Returns a list of TestResultDataEx insance from this tree. (shared!)
        """
        # Check each child (if any).
        aoRet = [];
        cChildErrors = 0;
        for oChild in self.aoChildren:
            if oChild.cErrors > 0:
                cChildErrors += oChild.cErrors;
                aoRet.extend(oChild.getListOfFailures());

        # Did we contribute as well?
        if self.cErrors > cChildErrors:
            aoRet.append(self);

        return aoRet;

    def getFullName(self):
        """ Constructs the full name of this test result. """
        if self.oParent is None:
            return self.sName;
        return self.oParent.getFullName() + ' / ' + self.sName;



class TestResultValueData(ModelDataBase):
    """
    Test result value data.
    """

    ksIdAttr    = 'idTestResultValue';

    ksParam_idTestResultValue   = 'TestResultValue_idTestResultValue';
    ksParam_idTestResult        = 'TestResultValue_idTestResult';
    ksParam_idTestSet           = 'TestResultValue_idTestSet';
    ksParam_tsCreated           = 'TestResultValue_tsCreated';
    ksParam_idStrName           = 'TestResultValue_idStrName';
    ksParam_lValue              = 'TestResultValue_lValue';
    ksParam_iUnit               = 'TestResultValue_iUnit';

    kasAllowNullAttributes      = [ 'idTestSet', ];

    def __init__(self):
        ModelDataBase.__init__(self)
        self.idTestResultValue  = None;
        self.idTestResult       = None;
        self.idTestSet          = None;
        self.tsCreated          = None;
        self.idStrName          = None;
        self.lValue             = None;
        self.iUnit              = 0;

    def initFromDbRow(self, aoRow):
        """
        Reinitialize from a SELECT * FROM TestResultValues.
        Return self. Raises exception if no row.
        """
        if aoRow is None:
            raise TMRowNotFound('Test result value record not found.')

        self.idTestResultValue  = aoRow[0];
        self.idTestResult       = aoRow[1];
        self.idTestSet          = aoRow[2];
        self.tsCreated          = aoRow[3];
        self.idStrName          = aoRow[4];
        self.lValue             = aoRow[5];
        self.iUnit              = aoRow[6];
        return self;


class TestResultValueDataEx(TestResultValueData):
    """
    Extends TestResultValue by resolving the value name and unit string.
    """

    def __init__(self):
        TestResultValueData.__init__(self)
        self.sName = None;
        self.sUnit = '';

    def initFromDbRow(self, aoRow):
        """
        Reinitialize from a query like this:
            SELECT TestResultValues.*, TestResultStrTab.sValue
            FROM TestResultValues, TestResultStrTab
            WHERE TestResultStrTab.idStr = TestResultValues.idStrName

        Return self. Raises exception if no row.
        """
        TestResultValueData.initFromDbRow(self, aoRow);
        self.sName = aoRow[7];
        if self.iUnit < len(constants.valueunit.g_asNames):
            self.sUnit = constants.valueunit.g_asNames[self.iUnit];
        else:
            self.sUnit = '<%d>' % (self.iUnit,);
        return self;

class TestResultMsgData(ModelDataBase):
    """
    Test result message data.
    """

    ksIdAttr    = 'idTestResultMsg';

    ksParam_idTestResultMsg     = 'TestResultValue_idTestResultMsg';
    ksParam_idTestResult        = 'TestResultValue_idTestResult';
    ksParam_idTestSet           = 'TestResultValue_idTestSet';
    ksParam_tsCreated           = 'TestResultValue_tsCreated';
    ksParam_idStrMsg            = 'TestResultValue_idStrMsg';
    ksParam_enmLevel            = 'TestResultValue_enmLevel';

    kasAllowNullAttributes      = [ 'idTestSet', ];

    kcDbColumns                 = 6

    def __init__(self):
        ModelDataBase.__init__(self)
        self.idTestResultMsg    = None;
        self.idTestResult       = None;
        self.idTestSet          = None;
        self.tsCreated          = None;
        self.idStrMsg           = None;
        self.enmLevel           = None;

    def initFromDbRow(self, aoRow):
        """
        Reinitialize from a SELECT * FROM TestResultMsgs.
        Return self. Raises exception if no row.
        """
        if aoRow is None:
            raise TMRowNotFound('Test result value record not found.')

        self.idTestResultMsg    = aoRow[0];
        self.idTestResult       = aoRow[1];
        self.idTestSet          = aoRow[2];
        self.tsCreated          = aoRow[3];
        self.idStrMsg           = aoRow[4];
        self.enmLevel           = aoRow[5];
        return self;

class TestResultMsgDataEx(TestResultMsgData):
    """
    Extends TestResultMsg by resolving the message string.
    """

    def __init__(self):
        TestResultMsgData.__init__(self)
        self.sMsg = None;

    def initFromDbRow(self, aoRow):
        """
        Reinitialize from a query like this:
            SELECT TestResultMsg.*, TestResultStrTab.sValue
            FROM   TestResultMsg, TestResultStrTab
            WHERE  TestResultStrTab.idStr = TestResultMsgs.idStrName

        Return self. Raises exception if no row.
        """
        TestResultMsgData.initFromDbRow(self, aoRow);
        self.sMsg = aoRow[self.kcDbColumns];
        return self;


class TestResultFileData(ModelDataBase):
    """
    Test result message data.
    """

    ksIdAttr    = 'idTestResultFile';

    ksParam_idTestResultFile    = 'TestResultFile_idTestResultFile';
    ksParam_idTestResult        = 'TestResultFile_idTestResult';
    ksParam_tsCreated           = 'TestResultFile_tsCreated';
    ksParam_idStrFile           = 'TestResultFile_idStrFile';
    ksParam_idStrDescription    = 'TestResultFile_idStrDescription';
    ksParam_idStrKind           = 'TestResultFile_idStrKind';
    ksParam_idStrMime           = 'TestResultFile_idStrMime';

    ## @name Kind of files.
    ## @{
    ksKind_LogReleaseVm         = 'log/release/vm';
    ksKind_LogDebugVm           = 'log/debug/vm';
    ksKind_LogReleaseSvc        = 'log/release/svc';
    ksKind_LogRebugSvc          = 'log/debug/svc';
    ksKind_LogReleaseClient     = 'log/release/client';
    ksKind_LogDebugClient       = 'log/debug/client';
    ksKind_LogInstaller         = 'log/installer';
    ksKind_LogUninstaller       = 'log/uninstaller';
    ksKind_LogGuestKernel       = 'log/guest/kernel';
    ksKind_CrashReportVm        = 'crash/report/vm';
    ksKind_CrashDumpVm          = 'crash/dump/vm';
    ksKind_CrashReportSvc       = 'crash/report/svc';
    ksKind_CrashDumpSvc         = 'crash/dump/svc';
    ksKind_CrashReportClient    = 'crash/report/client';
    ksKind_CrashDumpClient      = 'crash/dump/client';
    ksKind_InfoCollection       = 'info/collection';
    ksKind_InfoVgaText          = 'info/vgatext';
    ksKind_MiscOther            = 'misc/other';
    ksKind_ScreenshotFailure    = 'screenshot/failure';
    ksKind_ScreenshotSuccesss   = 'screenshot/success';
    #kSkind_ScreenCaptureFailure = 'screencapture/failure';
    ## @}

    kasAllowNullAttributes      = [ 'idTestSet', ];

    kcDbColumns                 =  8

    def __init__(self):
        ModelDataBase.__init__(self)
        self.idTestResultFile   = None;
        self.idTestResult       = None;
        self.idTestSet          = None;
        self.tsCreated          = None;
        self.idStrFile          = None;
        self.idStrDescription   = None;
        self.idStrKind          = None;
        self.idStrMime          = None;

    def initFromDbRow(self, aoRow):
        """
        Reinitialize from a SELECT * FROM TestResultFiles.
        Return self. Raises exception if no row.
        """
        if aoRow is None:
            raise TMRowNotFound('Test result file record not found.')

        self.idTestResultFile   = aoRow[0];
        self.idTestResult       = aoRow[1];
        self.idTestSet          = aoRow[2];
        self.tsCreated          = aoRow[3];
        self.idStrFile          = aoRow[4];
        self.idStrDescription   = aoRow[5];
        self.idStrKind          = aoRow[6];
        self.idStrMime          = aoRow[7];
        return self;

class TestResultFileDataEx(TestResultFileData):
    """
    Extends TestResultFile by resolving the strings.
    """

    def __init__(self):
        TestResultFileData.__init__(self)
        self.sFile          = None;
        self.sDescription   = None;
        self.sKind          = None;
        self.sMime          = None;

    def initFromDbRow(self, aoRow):
        """
        Reinitialize from a query like this:
            SELECT   TestResultFiles.*,
                     StrTabFile.sValue AS sFile,
                     StrTabDesc.sValue AS sDescription
                     StrTabKind.sValue AS sKind,
                     StrTabMime.sValue AS sMime,
            FROM ...

        Return self. Raises exception if no row.
        """
        TestResultFileData.initFromDbRow(self, aoRow);
        self.sFile          = aoRow[self.kcDbColumns];
        self.sDescription   = aoRow[self.kcDbColumns + 1];
        self.sKind          = aoRow[self.kcDbColumns + 2];
        self.sMime          = aoRow[self.kcDbColumns + 3];
        return self;

    def initFakeMainLog(self, oTestSet):
        """
        Reinitializes to represent the main.log object (not in DB).

        Returns self.
        """
        self.idTestResultFile   = 0;
        self.idTestResult       = oTestSet.idTestResult;
        self.tsCreated          = oTestSet.tsCreated;
        self.idStrFile          = None;
        self.idStrDescription   = None;
        self.idStrKind          = None;
        self.idStrMime          = None;

        self.sFile              = 'main.log';
        self.sDescription       = '';
        self.sKind              = 'log/main';
        self.sMime              = 'text/plain';
        return self;

    def isProbablyUtf8Encoded(self):
        """
        Checks if the file is likely to be UTF-8 encoded.
        """
        if self.sMime in [ 'text/plain', 'text/html' ]:
            return True;
        return False;

    def getMimeWithEncoding(self):
        """
        Gets the MIME type with encoding if likely to be UTF-8.
        """
        if self.isProbablyUtf8Encoded():
            return '%s; charset=utf-8' % (self.sMime,);
        return self.sMime;



class TestResultListingData(ModelDataBase): # pylint: disable=R0902
    """
    Test case result data representation for table listing
    """

    class FailureReasonListingData(object):
        """ Failure reason listing data """
        def __init__(self):
            self.oFailureReason          = None;
            self.oFailureReasonAssigner  = None;
            self.tsFailureReasonAssigned = None;
            self.sFailureReasonComment   = None;

    def __init__(self):
        """Initialize"""
        ModelDataBase.__init__(self)

        self.idTestSet               = None

        self.idBuildCategory         = None;
        self.sProduct                = None
        self.sRepository             = None;
        self.sBranch                 = None
        self.sType                   = None
        self.idBuild                 = None;
        self.sVersion                = None;
        self.iRevision               = None

        self.sOs                     = None;
        self.sOsVersion              = None;
        self.sArch                   = None;
        self.sCpuVendor              = None;
        self.sCpuName                = None;
        self.cCpus                   = None;
        self.fCpuHwVirt              = None;
        self.fCpuNestedPaging        = None;
        self.fCpu64BitGuest          = None;
        self.idTestBox               = None
        self.sTestBoxName            = None

        self.tsCreated               = None
        self.tsElapsed               = None
        self.enmStatus               = None
        self.cErrors                 = None;

        self.idTestCase              = None
        self.sTestCaseName           = None
        self.sBaseCmd                = None
        self.sArgs                   = None
        self.sSubName                = None;

        self.idBuildTestSuite        = None;
        self.iRevisionTestSuite      = None;

        self.aoFailureReasons        = [];

    def initFromDbRowEx(self, aoRow, oFailureReasonLogic, oUserAccountLogic):
        """
        Reinitialize from a database query.
        Return self. Raises exception if no row.
        """
        if aoRow is None:
            raise TMRowNotFound('Test result record not found.')

        self.idTestSet               = aoRow[0];

        self.idBuildCategory         = aoRow[1];
        self.sProduct                = aoRow[2];
        self.sRepository             = aoRow[3];
        self.sBranch                 = aoRow[4];
        self.sType                   = aoRow[5];
        self.idBuild                 = aoRow[6];
        self.sVersion                = aoRow[7];
        self.iRevision               = aoRow[8];

        self.sOs                     = aoRow[9];
        self.sOsVersion              = aoRow[10];
        self.sArch                   = aoRow[11];
        self.sCpuVendor              = aoRow[12];
        self.sCpuName                = aoRow[13];
        self.cCpus                   = aoRow[14];
        self.fCpuHwVirt              = aoRow[15];
        self.fCpuNestedPaging        = aoRow[16];
        self.fCpu64BitGuest          = aoRow[17];
        self.idTestBox               = aoRow[18];
        self.sTestBoxName            = aoRow[19];

        self.tsCreated               = aoRow[20];
        self.tsElapsed               = aoRow[21];
        self.enmStatus               = aoRow[22];
        self.cErrors                 = aoRow[23];

        self.idTestCase              = aoRow[24];
        self.sTestCaseName           = aoRow[25];
        self.sBaseCmd                = aoRow[26];
        self.sArgs                   = aoRow[27];
        self.sSubName                = aoRow[28];

        self.idBuildTestSuite        = aoRow[29];
        self.iRevisionTestSuite      = aoRow[30];

        self.aoFailureReasons         = [];
        for i, _ in enumerate(aoRow[31]):
            if   aoRow[31][i] is not None \
              or aoRow[32][i] is not None \
              or aoRow[33][i] is not None \
              or aoRow[34][i] is not None:
                oReason = self.FailureReasonListingData();
                if aoRow[31][i] is not None:
                    oReason.oFailureReason      = oFailureReasonLogic.cachedLookup(aoRow[31][i]);
                if aoRow[32][i] is not None:
                    oReason.oFailureReasonAssigner = oUserAccountLogic.cachedLookup(aoRow[32][i]);
                oReason.tsFailureReasonAssigned = aoRow[33][i];
                oReason.sFailureReasonComment   = aoRow[34][i];
                self.aoFailureReasons.append(oReason);

        return self


class TestResultHangingOffence(TMExceptionBase):
    """Hanging offence committed by test case."""
    pass;


class TestResultLogic(ModelLogicBase): # pylint: disable=R0903
    """
    Results grouped by scheduling group.
    """

    #
    # Result grinding for displaying in the WUI.
    #

    ksResultsGroupingTypeNone       = 'ResultsGroupingTypeNone';
    ksResultsGroupingTypeTestGroup  = 'ResultsGroupingTypeTestGroup';
    ksResultsGroupingTypeBuildRev   = 'ResultsGroupingTypeBuild';
    ksResultsGroupingTypeTestBox    = 'ResultsGroupingTypeTestBox';
    ksResultsGroupingTypeTestCase   = 'ResultsGroupingTypeTestCase';
    ksResultsGroupingTypeSchedGroup = 'ResultsGroupingTypeSchedGroup';

    ## @name Result sorting options.
    ## @{
    ksResultsSortByRunningAndStart      = 'ResultsSortByRunningAndStart'; ##< Default
    ksResultsSortByBuildRevision        = 'ResultsSortByBuildRevision';
    ksResultsSortByTestBoxName          = 'ResultsSortByTestBoxName';
    ksResultsSortByTestBoxOs            = 'ResultsSortByTestBoxOs';
    ksResultsSortByTestBoxOsVersion     = 'ResultsSortByTestBoxOsVersion';
    ksResultsSortByTestBoxOsArch        = 'ResultsSortByTestBoxOsArch';
    ksResultsSortByTestBoxArch          = 'ResultsSortByTestBoxArch';
    ksResultsSortByTestBoxCpuVendor     = 'ResultsSortByTestBoxCpuVendor';
    ksResultsSortByTestBoxCpuName       = 'ResultsSortByTestBoxCpuName';
    ksResultsSortByTestBoxCpuRev        = 'ResultsSortByTestBoxCpuRev';
    ksResultsSortByTestBoxCpuFeatures   = 'ResultsSortByTestBoxCpuFeatures';
    ksResultsSortByTestCaseName         = 'ResultsSortByTestCaseName';
    ksResultsSortByFailureReason        = 'ResultsSortByFailureReason';
    kasResultsSortBy = {
        ksResultsSortByRunningAndStart,
        ksResultsSortByBuildRevision,
        ksResultsSortByTestBoxName,
        ksResultsSortByTestBoxOs,
        ksResultsSortByTestBoxOsVersion,
        ksResultsSortByTestBoxOsArch,
        ksResultsSortByTestBoxArch,
        ksResultsSortByTestBoxCpuVendor,
        ksResultsSortByTestBoxCpuName,
        ksResultsSortByTestBoxCpuRev,
        ksResultsSortByTestBoxCpuFeatures,
        ksResultsSortByTestCaseName,
        ksResultsSortByFailureReason,
    };
    ## Used by the WUI for generating the drop down.
    kaasResultsSortByTitles = (
        ( ksResultsSortByRunningAndStart,       'Running & Start TS' ),
        ( ksResultsSortByBuildRevision,         'Build Revision' ),
        ( ksResultsSortByTestBoxName,           'TestBox Name' ),
        ( ksResultsSortByTestBoxOs,             'O/S' ),
        ( ksResultsSortByTestBoxOsVersion,      'O/S Version' ),
        ( ksResultsSortByTestBoxOsArch,         'O/S & Architecture' ),
        ( ksResultsSortByTestBoxArch,           'Architecture' ),
        ( ksResultsSortByTestBoxCpuVendor,      'CPU Vendor' ),
        ( ksResultsSortByTestBoxCpuName,        'CPU Vendor & Name' ),
        ( ksResultsSortByTestBoxCpuRev,         'CPU Vendor & Revision' ),
        ( ksResultsSortByTestBoxCpuFeatures,    'CPU Features' ),
        ( ksResultsSortByTestCaseName,          'Test Case Name' ),
        ( ksResultsSortByFailureReason,         'Failure Reason' ),
    );
    ## @}

    ## Default sort by map.
    kdResultSortByMap = {
        ksResultsSortByRunningAndStart:  ( '', None, None, '', '' ),
        ksResultsSortByBuildRevision: (
            # Sorting tables.
            ', Builds',
            # Sorting table join(s).
            ' AND TestSets.idBuild    = Builds.idBuild'
            ' AND Builds.tsExpire    >= TestSets.tsCreated'
            ' AND Builds.tsEffective <= TestSets.tsCreated',
            # Start of ORDER BY statement.
            ' Builds.iRevision DESC',
            # Extra columns to fetch for the above ORDER BY to work in a SELECT DISTINCT statement.
            '',
            # Columns for the GROUP BY
            ''),
        ksResultsSortByTestBoxName: (
            ', TestBoxes',
            ' AND TestSets.idGenTestBox = TestBoxes.idGenTestBox',
            ' TestBoxes.sName DESC',
            '', '' ),
        ksResultsSortByTestBoxOsArch: (
            ', TestBoxesWithStrings',
            ' AND TestSets.idGenTestBox = TestBoxesWithStrings.idGenTestBox',
            ' TestBoxesWithStrings.sOs, TestBoxesWithStrings.sCpuArch',
            '', ''  ),
        ksResultsSortByTestBoxOs: (
            ', TestBoxesWithStrings',
            ' AND TestSets.idGenTestBox = TestBoxesWithStrings.idGenTestBox',
            ' TestBoxesWithStrings.sOs',
            '', ''  ),
        ksResultsSortByTestBoxOsVersion: (
            ', TestBoxesWithStrings',
            ' AND TestSets.idGenTestBox = TestBoxesWithStrings.idGenTestBox',
            ' TestBoxesWithStrings.sOs, TestBoxesWithStrings.sOsVersion DESC',
            '', ''  ),
        ksResultsSortByTestBoxArch: (
            ', TestBoxesWithStrings',
            ' AND TestSets.idGenTestBox = TestBoxesWithStrings.idGenTestBox',
            ' TestBoxesWithStrings.sCpuArch',
            '', ''  ),
        ksResultsSortByTestBoxCpuVendor: (
            ', TestBoxesWithStrings',
            ' AND TestSets.idGenTestBox = TestBoxesWithStrings.idGenTestBox',
            ' TestBoxesWithStrings.sCpuVendor',
            '', ''  ),
        ksResultsSortByTestBoxCpuName: (
            ', TestBoxesWithStrings',
            ' AND TestSets.idGenTestBox = TestBoxesWithStrings.idGenTestBox',
            ' TestBoxesWithStrings.sCpuVendor, TestBoxesWithStrings.sCpuName',
            '', ''  ),
        ksResultsSortByTestBoxCpuRev: (
            ', TestBoxesWithStrings',
            ' AND TestSets.idGenTestBox = TestBoxesWithStrings.idGenTestBox',
            ' TestBoxesWithStrings.sCpuVendor, TestBoxesWithStrings.lCpuRevision DESC',
            ', TestBoxesWithStrings.lCpuRevision',
            ', TestBoxesWithStrings.lCpuRevision' ),
        ksResultsSortByTestBoxCpuFeatures: (
            ', TestBoxes',
            ' AND TestSets.idGenTestBox = TestBoxes.idGenTestBox',
            ' TestBoxes.fCpuHwVirt DESC, TestBoxes.fCpuNestedPaging DESC, TestBoxes.fCpu64BitGuest DESC, TestBoxes.cCpus DESC',
            '',
            '' ),
        ksResultsSortByTestCaseName: (
            ', TestCases',
            ' AND TestSets.idGenTestCase = TestCases.idGenTestCase',
            ' TestCases.sName',
            '', ''  ),
        ksResultsSortByFailureReason: (
            '', '',
            'asSortByFailureReason ASC',
            ', array_agg(FailureReasons.sShort ORDER BY TestResultFailures.idTestResult) AS asSortByFailureReason',
            '' ),
    };

    kdResultGroupingMap = {
        ksResultsGroupingTypeNone: (
            # Grouping tables;
            '',
            # Grouping field;
            None,
            # Grouping where addition.
            None,
            # Sort by overrides.
            {},
        ),
        ksResultsGroupingTypeTestGroup:  ('', 'TestSets.idTestGroup',     None,                      {},),
        ksResultsGroupingTypeTestBox:    ('', 'TestSets.idTestBox',       None,                      {},),
        ksResultsGroupingTypeTestCase:   ('', 'TestSets.idTestCase',      None,                      {},),
        ksResultsGroupingTypeBuildRev: (
            ', Builds',
            'Builds.iRevision',
            ' AND Builds.idBuild      = TestSets.idBuild'
            ' AND Builds.tsExpire     > TestSets.tsCreated'
            ' AND Builds.tsEffective <= TestSets.tsCreated',
            { ksResultsSortByBuildRevision: ( '', None,  ' Builds.iRevision DESC' ), }
        ),
        ksResultsGroupingTypeSchedGroup: ( '', 'TestSets.idSchedGroup',   None,                      {},),
    };


    def __init__(self, oDb):
        ModelLogicBase.__init__(self, oDb)
        self.oFailureReasonLogic = None;
        self.oUserAccountLogic   = None;

    def _getTimePeriodQueryPart(self, tsNow, sInterval, sExtraIndent = ''):
        """
        Get part of SQL query responsible for SELECT data within
        specified period of time.
        """
        assert sInterval is not None; # too many rows.

        cMonthsMourningPeriod = 2;  # Stop reminding everyone about testboxes after 2 months.  (May also speed up the query.)
        if tsNow is None:
            sRet =        '(TestSets.tsDone IS NULL OR TestSets.tsDone >= (CURRENT_TIMESTAMP - \'%s\'::interval))\n' \
                   '%s   AND TestSets.tsCreated >= (CURRENT_TIMESTAMP  - \'%s\'::interval - \'%u months\'::interval)\n' \
                 % ( sInterval,
                     sExtraIndent, sInterval, cMonthsMourningPeriod);
        else:
            sTsNow = '\'%s\'::TIMESTAMP' % (tsNow,); # It's actually a string already. duh.
            sRet =        'TestSets.tsCreated <= %s\n' \
                   '%s   AND TestSets.tsCreated >= (%s  - \'%s\'::interval - \'%u months\'::interval)\n' \
                   '%s   AND (TestSets.tsDone IS NULL OR TestSets.tsDone >= (%s - \'%s\'::interval))\n' \
                 % ( sTsNow,
                     sExtraIndent, sTsNow, sInterval, cMonthsMourningPeriod,
                     sExtraIndent, sTsNow, sInterval );
        return sRet

    def fetchResultsForListing(self, iStart, cMaxRows, tsNow, sInterval, enmResultSortBy, # pylint: disable=R0913
                               enmResultsGroupingType, iResultsGroupingValue, fOnlyFailures, fOnlyNeedingReason):
        """
        Fetches TestResults table content.

        If @param enmResultsGroupingType and @param iResultsGroupingValue
        are not None, then resulting (returned) list contains only records
        that match specified @param enmResultsGroupingType.

        If @param enmResultsGroupingType is None, then
        @param iResultsGroupingValue is ignored.

        Returns an array (list) of TestResultData items, empty list if none.
        Raises exception on error.
        """

        #
        # Get SQL query parameters
        #
        if enmResultsGroupingType is None or enmResultsGroupingType not in self.kdResultGroupingMap:
            raise TMExceptionBase('Unknown grouping type');
        if enmResultSortBy is None or enmResultSortBy not in self.kasResultsSortBy:
            raise TMExceptionBase('Unknown sorting');
        sGroupingTables, sGroupingField, sGroupingCondition, dSortingOverrides = self.kdResultGroupingMap[enmResultsGroupingType];
        if enmResultSortBy in dSortingOverrides:
            sSortTables, sSortWhere, sSortOrderBy, sSortColumns, sSortGroupBy = dSortingOverrides[enmResultSortBy];
        else:
            sSortTables, sSortWhere, sSortOrderBy, sSortColumns, sSortGroupBy = self.kdResultSortByMap[enmResultSortBy];

        #
        # Construct the query.
        #
        sQuery  = 'SELECT DISTINCT TestSets.idTestSet,\n' \
                  '       BuildCategories.idBuildCategory,\n' \
                  '       BuildCategories.sProduct,\n' \
                  '       BuildCategories.sRepository,\n' \
                  '       BuildCategories.sBranch,\n' \
                  '       BuildCategories.sType,\n' \
                  '       Builds.idBuild,\n' \
                  '       Builds.sVersion,\n' \
                  '       Builds.iRevision,\n' \
                  '       TestBoxesWithStrings.sOs,\n' \
                  '       TestBoxesWithStrings.sOsVersion,\n' \
                  '       TestBoxesWithStrings.sCpuArch,\n' \
                  '       TestBoxesWithStrings.sCpuVendor,\n' \
                  '       TestBoxesWithStrings.sCpuName,\n' \
                  '       TestBoxesWithStrings.cCpus,\n' \
                  '       TestBoxesWithStrings.fCpuHwVirt,\n' \
                  '       TestBoxesWithStrings.fCpuNestedPaging,\n' \
                  '       TestBoxesWithStrings.fCpu64BitGuest,\n' \
                  '       TestBoxesWithStrings.idTestBox,\n' \
                  '       TestBoxesWithStrings.sName,\n' \
                  '       TestResults.tsCreated,\n' \
                  '       COALESCE(TestResults.tsElapsed, CURRENT_TIMESTAMP - TestResults.tsCreated) AS tsElapsedTestResult,\n' \
                  '       TestSets.enmStatus,\n' \
                  '       TestResults.cErrors,\n' \
                  '       TestCases.idTestCase,\n' \
                  '       TestCases.sName,\n' \
                  '       TestCases.sBaseCmd,\n' \
                  '       TestCaseArgs.sArgs,\n' \
                  '       TestCaseArgs.sSubName,\n' \
                  '       TestSuiteBits.idBuild AS idBuildTestSuite,\n' \
                  '       TestSuiteBits.iRevision AS iRevisionTestSuite,\n' \
                  '       array_agg(TestResultFailures.idFailureReason ORDER BY TestResultFailures.idTestResult),\n' \
                  '       array_agg(TestResultFailures.uidAuthor       ORDER BY TestResultFailures.idTestResult),\n' \
                  '       array_agg(TestResultFailures.tsEffective     ORDER BY TestResultFailures.idTestResult),\n' \
                  '       array_agg(TestResultFailures.sComment        ORDER BY TestResultFailures.idTestResult),\n' \
                  '       (TestSets.tsDone IS NULL) SortRunningFirst' + sSortColumns + '\n' \
                  'FROM   (  SELECT TestSets.idTestSet AS idTestSet,\n' \
                  '                 TestSets.tsDone AS tsDone,\n' \
                  '                 TestSets.tsCreated AS tsCreated,\n' \
                  '                 TestSets.enmStatus AS enmStatus,\n' \
                  '                 TestSets.idBuild AS idBuild,\n' \
                  '                 TestSets.idBuildTestSuite AS idBuildTestSuite,\n' \
                  '                 TestSets.idGenTestBox AS idGenTestBox,\n' \
                  '                 TestSets.idGenTestCase AS idGenTestCase,\n' \
                  '                 TestSets.idGenTestCaseArgs AS idGenTestCaseArgs\n' \
                  '          FROM  TestSets';
        if fOnlyNeedingReason:
            sQuery += '\n' \
                      '          LEFT OUTER JOIN TestResultFailures\n' \
                      '                       ON     TestSets.idTestSet          = TestResultFailures.idTestSet\n' \
                      '                          AND TestResultFailures.tsExpire = \'infinity\'::TIMESTAMP';
        sQuery += sGroupingTables.replace(',', ',\n                ');
        sQuery += sSortTables.replace( ',', ',\n                ');
        sQuery += '\n' \
                  '          WHERE ' + self._getTimePeriodQueryPart(tsNow, sInterval, '         ');
        if fOnlyFailures or fOnlyNeedingReason:
            sQuery += '            AND TestSets.enmStatus != \'success\'::TestStatus_T\n' \
                      '            AND TestSets.enmStatus != \'running\'::TestStatus_T\n';
        if fOnlyNeedingReason:
            sQuery += '            AND TestResultFailures.idTestSet IS NULL\n';
        if sGroupingField is not None:
            sQuery += '            AND %s = %d\n' % (sGroupingField, iResultsGroupingValue,);
        if sGroupingCondition is not None:
            sQuery += sGroupingCondition.replace(' AND ', '            AND ');
        if sSortWhere is not None:
            sQuery += sSortWhere.replace(' AND ', '            AND ');
        sQuery += '          ORDER BY ';
        if sSortOrderBy is not None and sSortOrderBy.find('FailureReason') < 0:
            sQuery += sSortOrderBy + ',\n                ';
        sQuery += '(TestSets.tsDone IS NULL) DESC, TestSets.idTestSet DESC\n' \
                  '          LIMIT %s OFFSET %s\n' % (cMaxRows, iStart,);

        # Note! INNER JOIN TestBoxesWithStrings performs miserable compared to LEFT OUTER JOIN. Doesn't matter for the result
        #       because TestSets.idGenTestBox is a foreign key and unique in TestBoxes.  So, let's do what ever is faster.
        sQuery += '       ) AS TestSets\n' \
                  '       LEFT OUTER JOIN TestBoxesWithStrings\n' \
                  '                    ON TestSets.idGenTestBox     = TestBoxesWithStrings.idGenTestBox' \
                  '       LEFT OUTER JOIN Builds AS TestSuiteBits\n' \
                  '                    ON TestSets.idBuildTestSuite = TestSuiteBits.idBuild\n' \
                  '       LEFT OUTER JOIN TestResultFailures\n' \
                  '                    ON     TestSets.idTestSet          = TestResultFailures.idTestSet\n' \
                  '                       AND TestResultFailures.tsExpire = \'infinity\'::TIMESTAMP';
        if sSortOrderBy is not None and sSortOrderBy.find('FailureReason') >= 0:
            sQuery += '\n' \
                      '       LEFT OUTER JOIN FailureReasons\n' \
                      '                    ON     TestResultFailures.idFailureReason = FailureReasons.idFailureReason\n' \
                      '                       AND FailureReasons.tsExpire            = \'infinity\'::TIMESTAMP';
        sQuery += ',\n' \
                  '       BuildCategories,\n' \
                  '       Builds,\n' \
                  '       TestResults,\n' \
                  '       TestCases,\n' \
                  '       TestCaseArgs\n';
        sQuery += 'WHERE  TestSets.idTestSet         = TestResults.idTestSet\n' \
                  '   AND TestResults.idTestResultParent is NULL\n' \
                  '   AND TestSets.idBuild           = Builds.idBuild\n' \
                  '   AND Builds.tsExpire            > TestSets.tsCreated\n' \
                  '   AND Builds.tsEffective        <= TestSets.tsCreated\n' \
                  '   AND Builds.idBuildCategory     = BuildCategories.idBuildCategory\n' \
                  '   AND TestSets.idGenTestCase     = TestCases.idGenTestCase\n' \
                  '   AND TestSets.idGenTestCaseArgs = TestCaseArgs.idGenTestCaseArgs\n';
        sQuery += 'GROUP BY TestSets.idTestSet,\n' \
                  '         BuildCategories.idBuildCategory,\n' \
                  '         BuildCategories.sProduct,\n' \
                  '         BuildCategories.sRepository,\n' \
                  '         BuildCategories.sBranch,\n' \
                  '         BuildCategories.sType,\n' \
                  '         Builds.idBuild,\n' \
                  '         Builds.sVersion,\n' \
                  '         Builds.iRevision,\n' \
                  '         TestBoxesWithStrings.sOs,\n' \
                  '         TestBoxesWithStrings.sOsVersion,\n' \
                  '         TestBoxesWithStrings.sCpuArch,\n' \
                  '         TestBoxesWithStrings.sCpuVendor,\n' \
                  '         TestBoxesWithStrings.sCpuName,\n' \
                  '         TestBoxesWithStrings.cCpus,\n' \
                  '         TestBoxesWithStrings.fCpuHwVirt,\n' \
                  '         TestBoxesWithStrings.fCpuNestedPaging,\n' \
                  '         TestBoxesWithStrings.fCpu64BitGuest,\n' \
                  '         TestBoxesWithStrings.idTestBox,\n' \
                  '         TestBoxesWithStrings.sName,\n' \
                  '         TestResults.tsCreated,\n' \
                  '         tsElapsedTestResult,\n' \
                  '         TestSets.enmStatus,\n' \
                  '         TestResults.cErrors,\n' \
                  '         TestCases.idTestCase,\n' \
                  '         TestCases.sName,\n' \
                  '         TestCases.sBaseCmd,\n' \
                  '         TestCaseArgs.sArgs,\n' \
                  '         TestCaseArgs.sSubName,\n' \
                  '         TestSuiteBits.idBuild,\n' \
                  '         TestSuiteBits.iRevision,\n' \
                  '         SortRunningFirst' + sSortGroupBy + '\n';
        sQuery += 'ORDER BY ';
        if sSortOrderBy is not None:
            sQuery += sSortOrderBy.replace('TestBoxes.', 'TestBoxesWithStrings.') + ',\n       ';
        sQuery += '(TestSets.tsDone IS NULL) DESC, TestSets.idTestSet DESC\n';

        #
        # Execute the query and return the wrapped results.
        #
        self._oDb.execute(sQuery);

        if self.oFailureReasonLogic is None:
            self.oFailureReasonLogic = FailureReasonLogic(self._oDb);
        if self.oUserAccountLogic is None:
            self.oUserAccountLogic = UserAccountLogic(self._oDb);

        aoRows = [];
        for aoRow in self._oDb.fetchAll():
            aoRows.append(TestResultListingData().initFromDbRowEx(aoRow, self.oFailureReasonLogic, self.oUserAccountLogic));

        return aoRows


    def fetchTimestampsForLogViewer(self, idTestSet):
        """
        Returns an ordered list with all the test result timestamps, both start
        and end.

        The log viewer create anchors in the log text so we can jump directly to
        the log lines relevant for a test event.
        """
        self._oDb.execute('(\n'
                          'SELECT tsCreated\n'
                          'FROM   TestResults\n'
                          'WHERE  idTestSet = %s\n'
                          ') UNION (\n'
                          'SELECT tsCreated + tsElapsed\n'
                          'FROM   TestResults\n'
                          'WHERE  idTestSet = %s\n'
                          '   AND tsElapsed IS NOT NULL\n'
                          ') UNION (\n'
                          'SELECT TestResultFiles.tsCreated\n'
                          'FROM   TestResultFiles\n'
                          'WHERE  idTestSet = %s\n'
                          ') UNION (\n'
                          'SELECT tsCreated\n'
                          'FROM   TestResultValues\n'
                          'WHERE  idTestSet = %s\n'
                          ') UNION (\n'
                          'SELECT TestResultMsgs.tsCreated\n'
                          'FROM   TestResultMsgs\n'
                          'WHERE  idTestSet = %s\n'
                          ') ORDER by 1'
                          , ( idTestSet, idTestSet, idTestSet, idTestSet, idTestSet, ));
        return [aoRow[0] for aoRow in self._oDb.fetchAll()];


    def getEntriesCount(self, tsNow, sInterval, enmResultsGroupingType, iResultsGroupingValue, fOnlyFailures, fOnlyNeedingReason):
        """
        Get number of table records.

        If @param enmResultsGroupingType and @param iResultsGroupingValue
        are not None, then we count only only those records
        that match specified @param enmResultsGroupingType.

        If @param enmResultsGroupingType is None, then
        @param iResultsGroupingValue is ignored.
        """

        #
        # Get SQL query parameters
        #
        if enmResultsGroupingType is None:
            raise TMExceptionBase('Unknown grouping type')

        if enmResultsGroupingType not in self.kdResultGroupingMap:
            raise TMExceptionBase('Unknown grouping type')
        sGroupingTables, sGroupingField, sGroupingCondition, _  = self.kdResultGroupingMap[enmResultsGroupingType];

        #
        # Construct the query.
        #
        sQuery = 'SELECT COUNT(TestSets.idTestSet)\n' \
                 'FROM   TestSets';
        if fOnlyNeedingReason:
            sQuery += '\n' \
                      '       LEFT OUTER JOIN TestResultFailures\n' \
                      '                    ON     TestSets.idTestSet          = TestResultFailures.idTestSet\n' \
                      '                       AND TestResultFailures.tsExpire = \'infinity\'::TIMESTAMP';
        sQuery += sGroupingTables.replace(',', ',\n       ');
        sQuery += '\n' \
                  'WHERE  ' + self._getTimePeriodQueryPart(tsNow, sInterval);
        if fOnlyFailures or fOnlyNeedingReason:
            sQuery += '   AND TestSets.enmStatus != \'success\'::TestStatus_T\n' \
                      '   AND TestSets.enmStatus != \'running\'::TestStatus_T\n';
        if fOnlyNeedingReason:
            sQuery += '   AND TestResultFailures.idTestSet IS NULL\n';
        if sGroupingField is not None:
            sQuery += '   AND %s = %d\n' % (sGroupingField, iResultsGroupingValue,);
        if sGroupingCondition is not None:
            sQuery += sGroupingCondition.replace(' AND ', '   AND ');

        #
        # Execute the query and return the result.
        #
        self._oDb.execute(sQuery)
        return self._oDb.fetchOne()[0]

    def getTestGroups(self, tsNow, sPeriod):
        """
        Get list of uniq TestGroupData objects which
        found in all test results.
        """

        self._oDb.execute('SELECT DISTINCT TestGroups.*\n'
                          'FROM   TestGroups, TestSets\n'
                          'WHERE  TestSets.idTestGroup   =  TestGroups.idTestGroup\n'
                          '   AND TestGroups.tsExpire    >  TestSets.tsCreated\n'
                          '   AND TestGroups.tsEffective <= TestSets.tsCreated'
                          '   AND ' + self._getTimePeriodQueryPart(tsNow, sPeriod))
        aaoRows = self._oDb.fetchAll()
        aoRet = []
        for aoRow in aaoRows:
            aoRet.append(TestGroupData().initFromDbRow(aoRow))
        return aoRet

    def getBuilds(self, tsNow, sPeriod):
        """
        Get list of uniq BuildDataEx objects which
        found in all test results.
        """

        self._oDb.execute('SELECT DISTINCT Builds.*, BuildCategories.*\n'
                          'FROM     Builds, BuildCategories, TestSets\n'
                          'WHERE    TestSets.idBuild       =  Builds.idBuild\n'
                          '     AND Builds.idBuildCategory =  BuildCategories.idBuildCategory\n'
                          '     AND Builds.tsExpire        >  TestSets.tsCreated\n'
                          '     AND Builds.tsEffective     <= TestSets.tsCreated'
                          '     AND ' + self._getTimePeriodQueryPart(tsNow, sPeriod))
        aaoRows = self._oDb.fetchAll()
        aoRet = []
        for aoRow in aaoRows:
            aoRet.append(BuildDataEx().initFromDbRow(aoRow))
        return aoRet

    def getTestBoxes(self, tsNow, sPeriod):
        """
        Get list of uniq TestBoxData objects which
        found in all test results.
        """
        # Note! INNER JOIN TestBoxesWithStrings performs miserable compared to LEFT OUTER JOIN. Doesn't matter for the result
        #       because TestSets.idGenTestBox is a foreign key and unique in TestBoxes.  So, let's do what ever is faster.
        self._oDb.execute('SELECT TestBoxesWithStrings.*\n'
                          'FROM   ( SELECT idTestBox         AS idTestBox,\n'
                          '                MAX(idGenTestBox) AS idGenTestBox\n'
                          '         FROM   TestSets\n'
                          '         WHERE  ' + self._getTimePeriodQueryPart(tsNow, sPeriod, '        ') +
                          '         GROUP BY idTestBox\n'
                          '       ) AS TestBoxIDs\n'
                          '       LEFT OUTER JOIN TestBoxesWithStrings\n'
                          '                    ON TestBoxesWithStrings.idGenTestBox = TestBoxIDs.idGenTestBox\n'
                          'ORDER BY TestBoxesWithStrings.sName\n' );
        aoRet = []
        for aoRow in self._oDb.fetchAll():
            aoRet.append(TestBoxData().initFromDbRow(aoRow));
        return aoRet

    def getTestCases(self, tsNow, sPeriod):
        """
        Get a list of unique TestCaseData objects which is appears in the test
        specified result period.
        """

        # Using LEFT OUTER JOIN instead of INNER JOIN in case it performs better, doesn't matter for the result.
        self._oDb.execute('SELECT TestCases.*\n'
                          'FROM   ( SELECT idTestCase         AS idTestCase,\n'
                          '                MAX(idGenTestCase) AS idGenTestCase\n'
                          '         FROM   TestSets\n'
                          '         WHERE  ' + self._getTimePeriodQueryPart(tsNow, sPeriod, '        ') +
                          '         GROUP BY idTestCase\n'
                          '       ) AS TestCasesIDs\n'
                          '       LEFT OUTER JOIN TestCases ON TestCases.idGenTestCase = TestCasesIDs.idGenTestCase\n'
                          'ORDER BY TestCases.sName\n' );

        aoRet = [];
        for aoRow in self._oDb.fetchAll():
            aoRet.append(TestCaseData().initFromDbRow(aoRow));
        return aoRet

    def getSchedGroups(self, tsNow, sPeriod):
        """
        Get list of uniq SchedGroupData objects which
        found in all test results.
        """

        self._oDb.execute('SELECT SchedGroups.*\n'
                          'FROM   ( SELECT idSchedGroup,\n'
                          '                MAX(TestSets.tsCreated) AS tsNow\n'
                          '         FROM   TestSets\n'
                          '         WHERE  ' + self._getTimePeriodQueryPart(tsNow, sPeriod, '         ') +
                          '         GROUP BY idSchedGroup\n'
                          '       ) AS SchedGroupIDs\n'
                          '       INNER JOIN SchedGroups\n'
                          '               ON SchedGroups.idSchedGroup = SchedGroupIDs.idSchedGroup\n'
                          '              AND SchedGroups.tsExpire     > SchedGroupIDs.tsNow\n'
                          '              AND SchedGroups.tsEffective <= SchedGroupIDs.tsNow\n'
                          'ORDER BY SchedGroups.sName\n' );
        aoRet = []
        for aoRow in self._oDb.fetchAll():
            aoRet.append(SchedGroupData().initFromDbRow(aoRow));
        return aoRet

    def getById(self, idTestResult):
        """
        Get build record by its id
        """
        self._oDb.execute('SELECT *\n'
                          'FROM   TestResults\n'
                          'WHERE  idTestResult = %s\n',
                          (idTestResult,))

        aRows = self._oDb.fetchAll()
        if len(aRows) not in (0, 1):
            raise TMTooManyRows('Found more than one test result with the same credentials. Database structure is corrupted.')
        try:
            return TestResultData().initFromDbRow(aRows[0])
        except IndexError:
            return None


    #
    # Details view and interface.
    #

    def fetchResultTree(self, idTestSet, cMaxDepth = None):
        """
        Fetches the result tree for the given test set.

        Returns a tree of TestResultDataEx nodes.
        Raises exception on invalid input and database issues.
        """
        # Depth first, i.e. just like the XML added them.
        ## @todo this still isn't performing extremely well, consider optimizations.
        sQuery = self._oDb.formatBindArgs(
            'SELECT   TestResults.*,\n'
            '         TestResultStrTab.sValue,\n'
            '         EXISTS ( SELECT idTestResultValue\n'
            '           FROM   TestResultValues\n'
            '           WHERE  TestResultValues.idTestResult = TestResults.idTestResult ) AS fHasValues,\n'
            '         EXISTS ( SELECT idTestResultMsg\n'
            '           FROM   TestResultMsgs\n'
            '           WHERE  TestResultMsgs.idTestResult   = TestResults.idTestResult ) AS fHasMsgs,\n'
            '         EXISTS ( SELECT idTestResultFile\n'
            '           FROM   TestResultFiles\n'
            '           WHERE  TestResultFiles.idTestResult  = TestResults.idTestResult ) AS fHasFiles,\n'
            '         EXISTS ( SELECT idTestResult\n'
            '           FROM   TestResultFailures\n'
            '           WHERE  TestResultFailures.idTestResult = TestResults.idTestResult ) AS fHasReasons\n'
            'FROM     TestResults, TestResultStrTab\n'
            'WHERE    TestResults.idTestSet = %s\n'
            '     AND TestResults.idStrName = TestResultStrTab.idStr\n'
            , ( idTestSet, ));
        if cMaxDepth is not None:
            sQuery += self._oDb.formatBindArgs('     AND TestResults.iNestingDepth <= %s\n', (cMaxDepth,));
        sQuery += 'ORDER BY idTestResult ASC\n'

        self._oDb.execute(sQuery);
        cRows = self._oDb.getRowCount();
        if cRows > 65536:
            raise TMTooManyRows('Too many rows returned for idTestSet=%d: %d' % (idTestSet, cRows,));

        aaoRows = self._oDb.fetchAll();
        if len(aaoRows) == 0:
            raise TMRowNotFound('No test results for idTestSet=%d.' % (idTestSet,));

        # Set up the root node first.
        aoRow = aaoRows[0];
        oRoot = TestResultDataEx().initFromDbRow(aoRow);
        if oRoot.idTestResultParent is not None:
            raise self._oDb.integrityException('The root TestResult (#%s) has a parent (#%s)!'
                                               % (oRoot.idTestResult, oRoot.idTestResultParent));
        self._fetchResultTreeNodeExtras(oRoot, aoRow[-4], aoRow[-3], aoRow[-2], aoRow[-1]);

        # The chilren (if any).
        dLookup = { oRoot.idTestResult: oRoot };
        oParent = oRoot;
        for iRow in range(1, len(aaoRows)):
            aoRow = aaoRows[iRow];
            oCur = TestResultDataEx().initFromDbRow(aoRow);
            self._fetchResultTreeNodeExtras(oCur, aoRow[-4], aoRow[-3], aoRow[-2], aoRow[-1]);

            # Figure out and vet the parent.
            if oParent.idTestResult != oCur.idTestResultParent:
                oParent = dLookup.get(oCur.idTestResultParent, None);
                if oParent is None:
                    raise self._oDb.integrityException('TestResult #%d is orphaned from its parent #%s.'
                                                       % (oCur.idTestResult, oCur.idTestResultParent,));
            if oParent.iNestingDepth + 1 != oCur.iNestingDepth:
                raise self._oDb.integrityException('TestResult #%d has incorrect nesting depth (%d instead of %d)'
                                                   % (oCur.idTestResult, oCur.iNestingDepth, oParent.iNestingDepth + 1,));

            # Link it up.
            oCur.oParent = oParent;
            oParent.aoChildren.append(oCur);
            dLookup[oCur.idTestResult] = oCur;

        return (oRoot, dLookup);

    def _fetchResultTreeNodeExtras(self, oCurNode, fHasValues, fHasMsgs, fHasFiles, fHasReasons):
        """
        fetchResultTree worker that fetches values, message and files for the
        specified node.
        """
        assert(oCurNode.aoValues  == []);
        assert(oCurNode.aoMsgs    == []);
        assert(oCurNode.aoFiles   == []);
        assert(oCurNode.oReason is None);

        if fHasValues:
            self._oDb.execute('SELECT   TestResultValues.*,\n'
                              '         TestResultStrTab.sValue\n'
                              'FROM     TestResultValues, TestResultStrTab\n'
                              'WHERE    TestResultValues.idTestResult = %s\n'
                              '     AND TestResultValues.idStrName    = TestResultStrTab.idStr\n'
                              'ORDER BY idTestResultValue ASC\n'
                              , ( oCurNode.idTestResult, ));
            for aoRow in self._oDb.fetchAll():
                oCurNode.aoValues.append(TestResultValueDataEx().initFromDbRow(aoRow));

        if fHasMsgs:
            self._oDb.execute('SELECT   TestResultMsgs.*,\n'
                              '         TestResultStrTab.sValue\n'
                              'FROM     TestResultMsgs, TestResultStrTab\n'
                              'WHERE    TestResultMsgs.idTestResult = %s\n'
                              '     AND TestResultMsgs.idStrMsg     = TestResultStrTab.idStr\n'
                              'ORDER BY idTestResultMsg ASC\n'
                              , ( oCurNode.idTestResult, ));
            for aoRow in self._oDb.fetchAll():
                oCurNode.aoMsgs.append(TestResultMsgDataEx().initFromDbRow(aoRow));

        if fHasFiles:
            self._oDb.execute('SELECT   TestResultFiles.*,\n'
                              '         StrTabFile.sValue AS sFile,\n'
                              '         StrTabDesc.sValue AS sDescription,\n'
                              '         StrTabKind.sValue AS sKind,\n'
                              '         StrTabMime.sValue AS sMime\n'
                              'FROM     TestResultFiles,\n'
                              '         TestResultStrTab AS StrTabFile,\n'
                              '         TestResultStrTab AS StrTabDesc,\n'
                              '         TestResultStrTab AS StrTabKind,\n'
                              '         TestResultStrTab AS StrTabMime\n'
                              'WHERE    TestResultFiles.idTestResult     = %s\n'
                              '     AND TestResultFiles.idStrFile        = StrTabFile.idStr\n'
                              '     AND TestResultFiles.idStrDescription = StrTabDesc.idStr\n'
                              '     AND TestResultFiles.idStrKind        = StrTabKind.idStr\n'
                              '     AND TestResultFiles.idStrMime        = StrTabMime.idStr\n'
                              'ORDER BY idTestResultFile ASC\n'
                              , ( oCurNode.idTestResult, ));
            for aoRow in self._oDb.fetchAll():
                oCurNode.aoFiles.append(TestResultFileDataEx().initFromDbRow(aoRow));

        if fHasReasons or True:
            if self.oFailureReasonLogic is None:
                self.oFailureReasonLogic = FailureReasonLogic(self._oDb);
            if self.oUserAccountLogic is None:
                self.oUserAccountLogic = UserAccountLogic(self._oDb);
            self._oDb.execute('SELECT   *\n'
                              'FROM     TestResultFailures\n'
                              'WHERE    idTestResult = %s\n'
                              '     AND tsExpire = \'infinity\'::TIMESTAMP\n'
                              , ( oCurNode.idTestResult, ));
            if self._oDb.getRowCount() > 0:
                oCurNode.oReason = TestResultFailureDataEx().initFromDbRowEx(self._oDb.fetchOne(), self.oFailureReasonLogic,
                                                                             self.oUserAccountLogic);

        return True;



    #
    # TestBoxController interface(s).
    #

    def _inhumeTestResults(self, aoStack, idTestSet, sError):
        """
        The test produces too much output, kill and bury it.

        Note! We leave the test set open, only the test result records are
              completed.  Thus, _getResultStack will return an empty stack and
              cause XML processing to fail immediately, while we can still
              record when it actually completed in the test set the normal way.
        """
        self._oDb.dprint('** _inhumeTestResults: idTestSet=%d\n%s' % (idTestSet, self._stringifyStack(aoStack),));

        #
        # First add a message.
        #
        self._newFailureDetails(aoStack[0].idTestResult, idTestSet, sError, None);

        #
        # The complete all open test results.
        #
        for oTestResult in aoStack:
            oTestResult.cErrors += 1;
            self._completeTestResults(oTestResult, None, TestResultData.ksTestStatus_Failure, oTestResult.cErrors);

        # A bit of paranoia.
        self._oDb.execute('UPDATE TestResults\n'
                          'SET    cErrors = cErrors + 1,\n'
                          '       enmStatus = \'failure\'::TestStatus_T,\n'
                          '       tsElapsed = CURRENT_TIMESTAMP - tsCreated\n'
                          'WHERE  idTestSet = %s\n'
                          '   AND enmStatus = \'running\'::TestStatus_T\n'
                          , ( idTestSet, ));
        self._oDb.commit();

        return None;

    def strTabString(self, sString, fCommit = False):
        """
        Gets the string table id for the given string, adding it if new.

        Note! A copy of this code is also in TestSetLogic.
        """
        ## @todo move this and make a stored procedure for it.
        self._oDb.execute('SELECT   idStr\n'
                          'FROM     TestResultStrTab\n'
                          'WHERE    sValue = %s'
                          , (sString,));
        if self._oDb.getRowCount() == 0:
            self._oDb.execute('INSERT INTO TestResultStrTab (sValue)\n'
                              'VALUES   (%s)\n'
                              'RETURNING idStr\n'
                              , (sString,));
            if fCommit:
                self._oDb.commit();
        return self._oDb.fetchOne()[0];

    @staticmethod
    def _stringifyStack(aoStack):
        """Returns a string rep of the stack."""
        sRet = '';
        for i, _ in enumerate(aoStack):
            sRet += 'aoStack[%d]=%s\n' % (i, aoStack[i]);
        return sRet;

    def _getResultStack(self, idTestSet):
        """
        Gets the current stack of result sets.
        """
        self._oDb.execute('SELECT *\n'
                          'FROM   TestResults\n'
                          'WHERE  idTestSet = %s\n'
                          '   AND enmStatus = \'running\'::TestStatus_T\n'
                          'ORDER BY idTestResult DESC'
                          , ( idTestSet, ));
        aoStack = [];
        for aoRow in self._oDb.fetchAll():
            aoStack.append(TestResultData().initFromDbRow(aoRow));

        for i, _ in enumerate(aoStack):
            assert aoStack[i].iNestingDepth == len(aoStack) - i - 1, self._stringifyStack(aoStack);

        return aoStack;

    def _newTestResult(self, idTestResultParent, idTestSet, iNestingDepth, tsCreated, sName, dCounts, fCommit = False):
        """
        Creates a new test result.
        Returns the TestResultData object for the new record.
        May raise exception on database error.
        """
        assert idTestResultParent is not None;
        assert idTestResultParent > 1;

        #
        # This isn't necessarily very efficient, but it's necessary to prevent
        # a wild test or testbox from filling up the database.
        #
        sCountName = 'cTestResults';
        if sCountName not in dCounts:
            self._oDb.execute('SELECT   COUNT(idTestResult)\n'
                              'FROM     TestResults\n'
                              'WHERE    idTestSet = %s\n'
                              , ( idTestSet,));
            dCounts[sCountName] = self._oDb.fetchOne()[0];
        dCounts[sCountName] += 1;
        if dCounts[sCountName] > config.g_kcMaxTestResultsPerTS:
            raise TestResultHangingOffence('Too many sub-tests in total!');

        sCountName = 'cTestResultsIn%d' % (idTestResultParent,);
        if sCountName not in dCounts:
            self._oDb.execute('SELECT   COUNT(idTestResult)\n'
                              'FROM     TestResults\n'
                              'WHERE    idTestResultParent = %s\n'
                              , ( idTestResultParent,));
            dCounts[sCountName] = self._oDb.fetchOne()[0];
        dCounts[sCountName] += 1;
        if dCounts[sCountName] > config.g_kcMaxTestResultsPerTR:
            raise TestResultHangingOffence('Too many immediate sub-tests!');

        # This is also a hanging offence.
        if iNestingDepth > config.g_kcMaxTestResultDepth:
            raise TestResultHangingOffence('To deep sub-test nesting!');

        # Ditto.
        if len(sName) > config.g_kcchMaxTestResultName:
            raise TestResultHangingOffence('Test name is too long: %d chars - "%s"' % (len(sName), sName));

        #
        # Within bounds, do the job.
        #
        idStrName = self.strTabString(sName, fCommit);
        self._oDb.execute('INSERT INTO TestResults (\n'
                          '         idTestResultParent,\n'
                          '         idTestSet,\n'
                          '         tsCreated,\n'
                          '         idStrName,\n'
                          '         iNestingDepth )\n'
                          'VALUES (%s, %s, TIMESTAMP WITH TIME ZONE %s, %s, %s)\n'
                          'RETURNING *\n'
                          , ( idTestResultParent, idTestSet, tsCreated, idStrName, iNestingDepth) )
        oData = TestResultData().initFromDbRow(self._oDb.fetchOne());

        self._oDb.maybeCommit(fCommit);
        return oData;

    def _newTestValue(self, idTestResult, idTestSet, sName, lValue, sUnit, dCounts, tsCreated = None, fCommit = False):
        """
        Creates a test value.
        May raise exception on database error.
        """

        #
        # Bounds checking.
        #
        sCountName = 'cTestValues';
        if sCountName not in dCounts:
            self._oDb.execute('SELECT   COUNT(idTestResultValue)\n'
                              'FROM     TestResultValues, TestResults\n'
                              'WHERE    TestResultValues.idTestResult = TestResults.idTestResult\n'
                              '     AND TestResults.idTestSet = %s\n'
                              , ( idTestSet,));
            dCounts[sCountName] = self._oDb.fetchOne()[0];
        dCounts[sCountName] += 1;
        if dCounts[sCountName] > config.g_kcMaxTestValuesPerTS:
            raise TestResultHangingOffence('Too many values in total!');

        sCountName = 'cTestValuesIn%d' % (idTestResult,);
        if sCountName not in dCounts:
            self._oDb.execute('SELECT   COUNT(idTestResultValue)\n'
                              'FROM     TestResultValues\n'
                              'WHERE    idTestResult = %s\n'
                              , ( idTestResult,));
            dCounts[sCountName] = self._oDb.fetchOne()[0];
        dCounts[sCountName] += 1;
        if dCounts[sCountName] > config.g_kcMaxTestValuesPerTR:
            raise TestResultHangingOffence('Too many immediate values for one test result!');

        if len(sName) > config.g_kcchMaxTestValueName:
            raise TestResultHangingOffence('Value name is too long: %d chars - "%s"' % (len(sName), sName));

        #
        # Do the job.
        #
        iUnit = constants.valueunit.g_kdNameToConst.get(sUnit, constants.valueunit.NONE);

        idStrName = self.strTabString(sName, fCommit);
        if tsCreated is None:
            self._oDb.execute('INSERT INTO TestResultValues (\n'
                              '         idTestResult,\n'
                              '         idTestSet,\n'
                              '         idStrName,\n'
                              '         lValue,\n'
                              '         iUnit)\n'
                              'VALUES (  %s, %s, %s, %s, %s )\n'
                              , ( idTestResult, idTestSet, idStrName, lValue, iUnit,) );
        else:
            self._oDb.execute('INSERT INTO TestResultValues (\n'
                              '         idTestResult,\n'
                              '         idTestSet,\n'
                              '         tsCreated,\n'
                              '         idStrName,\n'
                              '         lValue,\n'
                              '         iUnit)\n'
                              'VALUES ( %s, %s, TIMESTAMP WITH TIME ZONE %s, %s, %s, %s )\n'
                              , ( idTestResult, idTestSet, tsCreated, idStrName, lValue, iUnit,) );
        self._oDb.maybeCommit(fCommit);
        return True;

    def _newFailureDetails(self, idTestResult, idTestSet, sText, dCounts, tsCreated = None, fCommit = False):
        """
        Creates a record detailing cause of failure.
        May raise exception on database error.
        """

        #
        # Overflow protection.
        #
        if dCounts is not None:
            sCountName = 'cTestMsgsIn%d' % (idTestResult,);
            if sCountName not in dCounts:
                self._oDb.execute('SELECT   COUNT(idTestResultMsg)\n'
                                  'FROM     TestResultMsgs\n'
                                  'WHERE    idTestResult = %s\n'
                                  , ( idTestResult,));
                dCounts[sCountName] = self._oDb.fetchOne()[0];
            dCounts[sCountName] += 1;
            if dCounts[sCountName] > config.g_kcMaxTestMsgsPerTR:
                raise TestResultHangingOffence('Too many messages under for one test result!');

            if len(sText) > config.g_kcchMaxTestMsg:
                raise TestResultHangingOffence('Failure details message is too long: %d chars - "%s"' % (len(sText), sText));

        #
        # Do the job.
        #
        idStrMsg = self.strTabString(sText, fCommit);
        if tsCreated is None:
            self._oDb.execute('INSERT INTO TestResultMsgs (\n'
                              '         idTestResult,\n'
                              '         idTestSet,\n'
                              '         idStrMsg,\n'
                              '         enmLevel)\n'
                              'VALUES ( %s, %s, %s, %s)\n'
                              , ( idTestResult, idTestSet, idStrMsg, 'failure',) );
        else:
            self._oDb.execute('INSERT INTO TestResultMsgs (\n'
                              '         idTestResult,\n'
                              '         idTestSet,\n'
                              '         tsCreated,\n'
                              '         idStrMsg,\n'
                              '         enmLevel)\n'
                              'VALUES ( %s, %s, TIMESTAMP WITH TIME ZONE %s, %s, %s)\n'
                              , ( idTestResult, idTestSet, tsCreated, idStrMsg, 'failure',) );

        self._oDb.maybeCommit(fCommit);
        return True;


    def _completeTestResults(self, oTestResult, tsDone, enmStatus, cErrors = 0, fCommit = False):
        """
        Completes a test result.  Updates the oTestResult object.
        May raise exception on database error.
        """
        self._oDb.dprint('** _completeTestResults: cErrors=%s tsDone=%s enmStatus=%s oTestResults=\n%s'
                         % (cErrors, tsDone, enmStatus, oTestResult,));

        #
        # Sanity check: No open sub tests (aoStack should make sure about this!).
        #
        self._oDb.execute('SELECT   COUNT(idTestResult)\n'
                          'FROM     TestResults\n'
                          'WHERE    idTestResultParent = %s\n'
                          '     AND enmStatus = %s\n'
                          , ( oTestResult.idTestResult, TestResultData.ksTestStatus_Running,));
        cOpenSubTest = self._oDb.fetchOne()[0];
        assert cOpenSubTest == 0, 'cOpenSubTest=%d - %s' % (cOpenSubTest, oTestResult,);
        assert oTestResult.enmStatus == TestResultData.ksTestStatus_Running;

        #
        # Make sure the reporter isn't lying about successes or error counts.
        #
        self._oDb.execute('SELECT   COALESCE(SUM(cErrors), 0)\n'
                          'FROM     TestResults\n'
                          'WHERE    idTestResultParent = %s\n'
                          , ( oTestResult.idTestResult, ));
        cMinErrors = self._oDb.fetchOne()[0] + oTestResult.cErrors;
        if cErrors < cMinErrors:
            cErrors = cMinErrors;
        if cErrors > 0 and enmStatus == TestResultData.ksTestStatus_Success:
            enmStatus = TestResultData.ksTestStatus_Failure

        #
        # Do the update.
        #
        if tsDone is None:
            self._oDb.execute('UPDATE   TestResults\n'
                              'SET      cErrors = %s,\n'
                              '         enmStatus = %s,\n'
                              '         tsElapsed = CURRENT_TIMESTAMP - tsCreated\n'
                              'WHERE    idTestResult = %s\n'
                              'RETURNING tsElapsed'
                              , ( cErrors, enmStatus, oTestResult.idTestResult,) );
        else:
            self._oDb.execute('UPDATE   TestResults\n'
                              'SET      cErrors = %s,\n'
                              '         enmStatus = %s,\n'
                              '         tsElapsed = TIMESTAMP WITH TIME ZONE %s - tsCreated\n'
                              'WHERE    idTestResult = %s\n'
                              'RETURNING tsElapsed'
                              , ( cErrors, enmStatus, tsDone, oTestResult.idTestResult,) );

        oTestResult.tsElapsed = self._oDb.fetchOne()[0];
        oTestResult.enmStatus = enmStatus;
        oTestResult.cErrors   = cErrors;

        self._oDb.maybeCommit(fCommit);
        return None;

    def _doPopHint(self, aoStack, cStackEntries, dCounts, idTestSet):
        """ Executes a PopHint. """
        assert cStackEntries >= 0;
        while len(aoStack) > cStackEntries:
            if aoStack[0].enmStatus == TestResultData.ksTestStatus_Running:
                self._newFailureDetails(aoStack[0].idTestResult, idTestSet, 'XML error: Missing </Test>', dCounts);
                self._completeTestResults(aoStack[0], tsDone = None, cErrors = 1,
                                          enmStatus = TestResultData.ksTestStatus_Failure, fCommit = True);
            aoStack.pop(0);
        return True;


    @staticmethod
    def _validateElement(sName, dAttribs, fClosed):
        """
        Validates an element and its attributes.
        """

        #
        # Validate attributes by name.
        #

        # Validate integer attributes.
        for sAttr in [ 'errors', 'testdepth' ]:
            if sAttr in dAttribs:
                try:
                    _ = int(dAttribs[sAttr]);
                except:
                    return 'Element %s has an invalid %s attribute value: %s.' % (sName, sAttr, dAttribs[sAttr],);

        # Validate long attributes.
        for sAttr in [ 'value', ]:
            if sAttr in dAttribs:
                try:
                    _ = long(dAttribs[sAttr]);  # pylint: disable=R0204
                except:
                    return 'Element %s has an invalid %s attribute value: %s.' % (sName, sAttr, dAttribs[sAttr],);

        # Validate string attributes.
        for sAttr in [ 'name', 'text' ]: # 'unit' can be zero length.
            if sAttr in dAttribs and len(dAttribs[sAttr]) == 0:
                return 'Element %s has an empty %s attribute value.' % (sName, sAttr,);

        # Validate the timestamp attribute.
        if 'timestamp' in dAttribs:
            (dAttribs['timestamp'], sError) = ModelDataBase.validateTs(dAttribs['timestamp'], fAllowNull = False);
            if sError is not None:
                return 'Element %s has an invalid timestamp ("%s"): %s' % (sName, dAttribs['timestamp'], sError,);


        #
        # Check that attributes that are required are present.
        # We ignore extra attributes.
        #
        dElementAttribs = \
        {
            'Test':             [ 'timestamp', 'name', ],
            'Value':            [ 'timestamp', 'name', 'unit', 'value', ],
            'FailureDetails':   [ 'timestamp', 'text', ],
            'Passed':           [ 'timestamp', ],
            'Skipped':          [ 'timestamp', ],
            'Failed':           [ 'timestamp', 'errors', ],
            'TimedOut':         [ 'timestamp', 'errors', ],
            'End':              [ 'timestamp', ],
            'PushHint':         [ 'testdepth', ],
            'PopHint':          [ 'testdepth', ],
        };
        if sName not in dElementAttribs:
            return 'Unknown element "%s".' % (sName,);
        for sAttr in dElementAttribs[sName]:
            if sAttr not in dAttribs:
                return 'Element %s requires attribute "%s".' % (sName, sAttr);

        #
        # Only the Test element can (and must) remain open.
        #
        if sName == 'Test' and fClosed:
            return '<Test/> is not allowed.';
        if sName != 'Test' and not fClosed:
            return 'All elements except <Test> must be closed.';

        return None;

    @staticmethod
    def _parseElement(sElement):
        """
        Parses an element.

        """
        #
        # Element level bits.
        #
        sName    = sElement.split()[0];
        sElement = sElement[len(sName):];

        fClosed  = sElement[-1] == '/';
        if fClosed:
            sElement = sElement[:-1];

        #
        # Attributes.
        #
        sError   = None;
        dAttribs = {};
        sElement = sElement.strip();
        while len(sElement) > 0:
            # Extract attribute name.
            off = sElement.find('=');
            if off < 0 or not sElement[:off].isalnum():
                sError = 'Attributes shall have alpha numberical names and have values.';
                break;
            sAttr = sElement[:off];

            # Extract attribute value.
            if off + 2 >= len(sElement) or sElement[off + 1] != '"':
                sError = 'Attribute (%s) value is missing or not in double quotes.' % (sAttr,);
                break;
            off += 2;
            offEndQuote = sElement.find('"', off);
            if offEndQuote < 0:
                sError = 'Attribute (%s) value is missing end quotation mark.' % (sAttr,);
                break;
            sValue = sElement[off:offEndQuote];

            # Check for duplicates.
            if sAttr in dAttribs:
                sError = 'Attribute "%s" appears more than once.' % (sAttr,);
                break;

            # Unescape the value.
            sValue = sValue.replace('&lt;',   '<');
            sValue = sValue.replace('&gt;',   '>');
            sValue = sValue.replace('&apos;', '\'');
            sValue = sValue.replace('&quot;', '"');
            sValue = sValue.replace('&#xA;',  '\n');
            sValue = sValue.replace('&#xD;',  '\r');
            sValue = sValue.replace('&amp;',  '&'); # last

            # Done.
            dAttribs[sAttr] = sValue;

            # advance
            sElement = sElement[offEndQuote + 1:];
            sElement = sElement.lstrip();

        #
        # Validate the element before we return.
        #
        if sError is None:
            sError = TestResultLogic._validateElement(sName, dAttribs, fClosed);

        return (sName, dAttribs, sError)

    def _handleElement(self, sName, dAttribs, idTestSet, aoStack, aaiHints, dCounts):
        """
        Worker for processXmlStream that handles one element.

        Returns None on success, error string on bad XML or similar.
        Raises exception on hanging offence and on database error.
        """
        if sName == 'Test':
            iNestingDepth = aoStack[0].iNestingDepth + 1 if len(aoStack) > 0 else 0;
            aoStack.insert(0, self._newTestResult(idTestResultParent = aoStack[0].idTestResult, idTestSet = idTestSet,
                                                  tsCreated = dAttribs['timestamp'], sName = dAttribs['name'],
                                                  iNestingDepth = iNestingDepth, dCounts = dCounts, fCommit = True) );

        elif sName == 'Value':
            self._newTestValue(idTestResult = aoStack[0].idTestResult, idTestSet = idTestSet, tsCreated = dAttribs['timestamp'],
                               sName = dAttribs['name'], sUnit = dAttribs['unit'], lValue = long(dAttribs['value']),
                               dCounts = dCounts, fCommit = True);

        elif sName == 'FailureDetails':
            self._newFailureDetails(idTestResult = aoStack[0].idTestResult, idTestSet = idTestSet,
                                    tsCreated = dAttribs['timestamp'], sText = dAttribs['text'], dCounts = dCounts,
                                    fCommit = True);

        elif sName == 'Passed':
            self._completeTestResults(aoStack[0], tsDone = dAttribs['timestamp'],
                                      enmStatus = TestResultData.ksTestStatus_Success, fCommit = True);

        elif sName == 'Skipped':
            self._completeTestResults(aoStack[0], tsDone = dAttribs['timestamp'],
                                      enmStatus = TestResultData.ksTestStatus_Skipped, fCommit = True);

        elif sName == 'Failed':
            self._completeTestResults(aoStack[0], tsDone = dAttribs['timestamp'], cErrors = int(dAttribs['errors']),
                                      enmStatus = TestResultData.ksTestStatus_Failure, fCommit = True);

        elif sName == 'TimedOut':
            self._completeTestResults(aoStack[0], tsDone = dAttribs['timestamp'], cErrors = int(dAttribs['errors']),
                                      enmStatus = TestResultData.ksTestStatus_TimedOut, fCommit = True);

        elif sName == 'End':
            self._completeTestResults(aoStack[0], tsDone = dAttribs['timestamp'],
                                      cErrors = int(dAttribs.get('errors', '1')),
                                      enmStatus = TestResultData.ksTestStatus_Success, fCommit = True);

        elif sName == 'PushHint':
            if len(aaiHints) > 1:
                return 'PushHint cannot be nested.'

            aaiHints.insert(0, [len(aoStack), int(dAttribs['testdepth'])]);

        elif sName == 'PopHint':
            if len(aaiHints) < 1:
                return 'No hint to pop.'

            iDesiredTestDepth = int(dAttribs['testdepth']);
            cStackEntries, iTestDepth = aaiHints.pop(0);
            self._doPopHint(aoStack, cStackEntries, dCounts, idTestSet); # Fake the necessary '<End/></Test>' tags.
            if iDesiredTestDepth != iTestDepth:
                return 'PopHint tag has different testdepth: %d, on stack %d.' % (iDesiredTestDepth, iTestDepth);
        else:
            return 'Unexpected element "%s".' % (sName,);
        return None;


    def processXmlStream(self, sXml, idTestSet):
        """
        Processes the "XML" stream section given in sXml.

        The sXml isn't a complete XML document, even should we save up all sXml
        for a given set, they may not form a complete and well formed XML
        document since the test may be aborted, abend or simply be buggy. We
        therefore do our own parsing and treat the XML tags as commands more
        than anything else.

        Returns (sError, fUnforgivable), where sError is None on success.
        May raise database exception.
        """
        aoStack    = self._getResultStack(idTestSet); # [0] == top; [-1] == bottom.
        if len(aoStack) == 0:
            return ('No open results', True);
        self._oDb.dprint('** processXmlStream len(aoStack)=%s' % (len(aoStack),));
        #self._oDb.dprint('processXmlStream: %s' % (self._stringifyStack(aoStack),));
        #self._oDb.dprint('processXmlStream: sXml=%s' % (sXml,));

        dCounts    = {};
        aaiHints   = [];
        sError     = None;

        fExpectCloseTest = False;
        sXml = sXml.strip();
        while len(sXml) > 0:
            if sXml.startswith('</Test>'): # Only closing tag.
                offNext = len('</Test>');
                if len(aoStack) <= 1:
                    sError = 'Trying to close the top test results.'
                    break;
                # ASSUMES that we've just seen an <End/>, <Passed/>, <Failed/>,
                # <TimedOut/> or <Skipped/> tag earlier in this call!
                if aoStack[0].enmStatus == TestResultData.ksTestStatus_Running  or  not fExpectCloseTest:
                    sError = 'Missing <End/>, <Passed/>, <Failed/>, <TimedOut/> or <Skipped/> tag.';
                    break;
                aoStack.pop(0);
                fExpectCloseTest = False;

            elif fExpectCloseTest:
                sError = 'Expected </Test>.'
                break;

            elif   sXml.startswith('<?xml '):  # Ignore (included files).
                offNext = sXml.find('?>');
                if offNext < 0:
                    sError = 'Unterminated <?xml ?> element.';
                    break;
                offNext += 2;

            elif sXml[0] == '<':
                # Parse and check the tag.
                if not sXml[1].isalpha():
                    sError = 'Malformed element.';
                    break;
                offNext = sXml.find('>')
                if offNext < 0:
                    sError = 'Unterminated element.';
                    break;
                (sName, dAttribs, sError) = self._parseElement(sXml[1:offNext]);
                offNext += 1;
                if sError is not None:
                    break;

                # Handle it.
                try:
                    sError = self._handleElement(sName, dAttribs, idTestSet, aoStack, aaiHints, dCounts);
                except TestResultHangingOffence as oXcpt:
                    self._inhumeTestResults(aoStack, idTestSet, str(oXcpt));
                    return (str(oXcpt), True);


                fExpectCloseTest = sName in [ 'End', 'Passed', 'Failed', 'TimedOut', 'Skipped', ];
            else:
                sError = 'Unexpected content.';
                break;

            # Advance.
            sXml = sXml[offNext:];
            sXml = sXml.lstrip();

        #
        # Post processing checks.
        #
        if sError is None and fExpectCloseTest:
            sError = 'Expected </Test> before the end of the XML section.'
        elif sError is None and len(aaiHints) > 0:
            sError = 'Expected </PopHint> before the end of the XML section.'
        if len(aaiHints) > 0:
            self._doPopHint(aoStack, aaiHints[-1][0], dCounts, idTestSet);

        #
        # Log the error.
        #
        if sError is not None:
            SystemLogLogic(self._oDb).addEntry(SystemLogData.ksEvent_XmlResultMalformed,
                                               'idTestSet=%s idTestResult=%s XML="%s" %s'
                                               % ( idTestSet,
                                                   aoStack[0].idTestResult if len(aoStack) > 0 else -1,
                                                   sXml[:30 if len(sXml) >= 30 else len(sXml)],
                                                   sError, ),
                                               cHoursRepeat = 6, fCommit = True);
        return (sError, False);





#
# Unit testing.
#

# pylint: disable=C0111
class TestResultDataTestCase(ModelDataBaseTestCase):
    def setUp(self):
        self.aoSamples = [TestResultData(),];

class TestResultValueDataTestCase(ModelDataBaseTestCase):
    def setUp(self):
        self.aoSamples = [TestResultValueData(),];

if __name__ == '__main__':
    unittest.main();
    # not reached.

