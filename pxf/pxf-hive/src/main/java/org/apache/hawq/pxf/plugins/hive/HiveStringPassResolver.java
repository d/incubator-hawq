package org.apache.hawq.pxf.plugins.hive;

/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */


import org.apache.hawq.pxf.api.OneField;
import org.apache.hawq.pxf.api.OneRow;
import org.apache.hawq.pxf.api.utilities.InputData;

import java.util.Collections;
import java.util.List;

import static org.apache.hawq.pxf.api.io.DataType.VARCHAR;

/**
 * Specialized HiveResolver for a Hive table stored as Text files.
 * Use together with HiveInputFormatFragmenter/HiveLineBreakAccessor.
 */
public class HiveStringPassResolver extends HiveResolver {
    private StringBuilder parts;

    public HiveStringPassResolver(InputData input) throws Exception {
        super(input);
    }

    @Override
    void parseUserData(InputData input) throws Exception {
        String userData = new String(input.getFragmentUserData());
        String[] toks = userData.split(HiveDataFragmenter.HIVE_UD_DELIM);
        parseDelimiterChar(input);
        parts = new StringBuilder();
        partitionKeys = toks[HiveInputFormatFragmenter.TOK_KEYS];
    }

    @Override
    void initSerde(InputData input) {
        /* nothing to do here */
    }

    @Override
    void initPartitionFields() {
        initPartitionFields(parts);
    }

    /**
     * getFields returns a singleton list of OneField item.
     * OneField item contains two fields: an integer representing the VARCHAR type and a Java
     * Object representing the field value.
     */
    @Override
    public List<OneField> getFields(OneRow onerow) throws Exception {
        String line = (onerow.getData()).toString();

        /* We follow Hive convention. Partition fields are always added at the end of the record */
        return Collections.singletonList(new OneField(VARCHAR.getOID(), line + parts));
    }
}
